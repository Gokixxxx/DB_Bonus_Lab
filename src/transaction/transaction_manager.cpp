/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "index/ix.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn == nullptr) {
        txn_id_t new_id = next_txn_id_.fetch_add(1);
        txn = new Transaction(new_id);
        txn->set_start_ts(next_timestamp_.fetch_add(1));
    }
    txn->set_state(TransactionState::GROWING);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }

    auto lock_set = txn->get_lock_set();
    for (const auto &lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();

    txn->set_state(TransactionState::COMMITTED);

    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
    delete txn;
}

void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *wr = write_set->back();
        write_set->pop_back();
        const std::string &tab_name = wr->GetTableName();
        const Rid &rid = wr->GetRid();

        try {
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) {
                delete wr;
                continue;
            }
            RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);

            switch (wr->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    auto rec = fh->get_record(rid, nullptr);
                    for (auto &index : tab.indexes) {
                        auto ih = sm_manager_->ihs_.at(
                            sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                            memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                            offset += index.cols[j].len;
                        }
                        ih->delete_entry(key, txn);
                        delete[] key;
                    }
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    fh->insert_record(rid, wr->GetRecord().data);
                    for (auto &index : tab.indexes) {
                        auto ih = sm_manager_->ihs_.at(
                            sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                            memcpy(key + offset,
                                   wr->GetRecord().data + index.cols[j].offset,
                                   index.cols[j].len);
                            offset += index.cols[j].len;
                        }
                        ih->insert_entry(key, rid, txn);
                        delete[] key;
                    }
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    auto cur_rec = fh->get_record(rid, nullptr);
                    for (auto &index : tab.indexes) {
                        auto ih = sm_manager_->ihs_.at(
                            sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                        char *cur_key = new char[index.col_tot_len];
                        int offset = 0;
                        for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                            memcpy(cur_key + offset, cur_rec->data + index.cols[j].offset, index.cols[j].len);
                            offset += index.cols[j].len;
                        }
                        ih->delete_entry(cur_key, txn);
                        delete[] cur_key;
                    }
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    for (auto &index : tab.indexes) {
                        auto ih = sm_manager_->ihs_.at(
                            sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                        char *old_key = new char[index.col_tot_len];
                        int offset = 0;
                        for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                            memcpy(old_key + offset,
                                   wr->GetRecord().data + index.cols[j].offset,
                                   index.cols[j].len);
                            offset += index.cols[j].len;
                        }
                        ih->insert_entry(old_key, rid, txn);
                        delete[] old_key;
                    }
                    break;
                }
            }
        } catch (...) {
            // 回滚失败直接跳过，保证锁能释放
        }
        delete wr;
    }

    auto lock_set = txn->get_lock_set();
    for (const auto &lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();

    txn->set_state(TransactionState::ABORTED);

    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
    delete txn;
}