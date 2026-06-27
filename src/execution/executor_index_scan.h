/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include <algorithm>
#include <map>

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        // 表级 IS 锁（和 IX 兼容，读写可以并发）
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_IS_on_table(context_->txn_, fh_->GetFd());
        }

        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_)).get();

        std::map<std::string, const Condition*> eq_conds;    
        std::map<std::string, const Condition*> lower_conds; 
        std::map<std::string, const Condition*> upper_conds; 
        
        for (auto &cond : fed_conds_) {
            if (!cond.is_rhs_val) continue;
            const std::string &name = cond.lhs_col.col_name;
            
            bool is_idx = false;
            for (auto &col : index_meta_.cols) {
                if (col.name == name) {
                    is_idx = true;
                    break;
                }
            }
            if (!is_idx) continue;

            switch (cond.op) {
                case OP_EQ: eq_conds[name] = &cond; break;
                case OP_GE: case OP_GT: lower_conds[name] = &cond; break;
                case OP_LE: case OP_LT: upper_conds[name] = &cond; break;
                default: break;
            }
        }

        std::vector<char> lower_key(index_meta_.col_tot_len, 0x00);
        std::vector<char> upper_key(index_meta_.col_tot_len, static_cast<char>(0xFF));
        
        bool use_index_lookup = false;
        int current_offset = 0;

        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &col = index_meta_.cols[i];
            
            auto it_eq = eq_conds.find(col.name);
            if (it_eq != eq_conds.end()) {
                memcpy(lower_key.data() + current_offset, it_eq->second->rhs_val.raw->data, col.len);
                memcpy(upper_key.data() + current_offset, it_eq->second->rhs_val.raw->data, col.len);
                current_offset += col.len;
                use_index_lookup = true;
                continue; 
            }
            
            auto it_lo = lower_conds.find(col.name);
            if (it_lo != lower_conds.end()) {
                memcpy(lower_key.data() + current_offset, it_lo->second->rhs_val.raw->data, col.len);
                use_index_lookup = true;
            }
            
            auto it_up = upper_conds.find(col.name);
            if (it_up != upper_conds.end()) {
                memcpy(upper_key.data() + current_offset, it_up->second->rhs_val.raw->data, col.len);
                use_index_lookup = true;
            }
            break;
        }

        Iid lower = ih->leaf_begin();
        Iid upper = ih->leaf_end();
        if (use_index_lookup) {
            lower = ih->lower_bound(lower_key.data());
            upper = ih->upper_bound(upper_key.data());
        }

        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            // 行级 S 锁
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
                context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid_, fh_->GetFd());
            }
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                break;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        for (scan_->next(); !scan_->is_end(); scan_->next()) {
            rid_ = scan_->rid();
            // 行级 S 锁
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
                context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid_, fh_->GetFd());
            }
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                break;
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

private:
    bool eval_cond(const std::vector<ColMeta> &rec_cols, const Condition &cond, const RmRecord *rec) {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        char *lhs_data = rec->data + lhs_col->offset;
        char *rhs_data;
        ColType rhs_type;
        int cmp_len = lhs_col->len;

        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs_data = rec->data + rhs_col->offset;
            rhs_type = rhs_col->type;
            cmp_len = std::min(lhs_col->len, rhs_col->len);
        }

        int cmp = 0;
        if (lhs_col->type == TYPE_INT && rhs_type == TYPE_INT) {
            int lv = *(int *)lhs_data, rv = *(int *)rhs_data;
            cmp = (lv > rv) - (lv < rv);
        } else if (lhs_col->type == TYPE_FLOAT && rhs_type == TYPE_FLOAT) {
            float lv = *(float *)lhs_data, rv = *(float *)rhs_data;
            cmp = (lv > rv) - (lv < rv);
        } else if (lhs_col->type == TYPE_STRING && rhs_type == TYPE_STRING) {
            cmp = memcmp(lhs_data, rhs_data, cmp_len);
        } else if (lhs_col->type == TYPE_INT && rhs_type == TYPE_FLOAT) {
            float lv = (float)(*(int *)lhs_data), rv = *(float *)rhs_data;
            cmp = (lv > rv) - (lv < rv);
        } else if (lhs_col->type == TYPE_FLOAT && rhs_type == TYPE_INT) {
            float lv = *(float *)lhs_data, rv = (float)(*(int *)rhs_data);
            cmp = (lv > rv) - (lv < rv);
        }

        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

    bool eval_conds(const std::vector<ColMeta> &rec_cols, const std::vector<Condition> &conds, const RmRecord *rec) {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, cond, rec)) return false;
        }
        return true;
    }

    Rid &rid() override { return rid_; }
};