/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

/**
 * @brief 
 * @todo 加上读锁（需要使用缓冲池得到page）
 */
void IxScan::next() {
    assert(!is_end());
    IxNodeHandle *node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    assert(iid_.slot_no < node->get_size());
    
    iid_.slot_no++;
    if (iid_.page_no != ih_->file_hdr_->last_leaf_ && iid_.slot_no == node->get_size()) {
        page_id_t next = node->get_next_leaf();
        if (next != IX_NO_PAGE && next != IX_LEAF_HEADER_PAGE) {
            iid_.slot_no = 0;
            iid_.page_no = next;
        }
        // 否则：当前已经是有效遍历的末尾，下次 is_end() 会检测到 iid_ == end_
    }

    ih_->buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
}

Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}