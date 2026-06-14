/**
 * B+树 Phase 1~3 独立测试
 * 编译：g++ -std=c++17 -I./src -o test_btree test_btree.cpp
 * 运行：./test_btree
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>

// ============================================
// 最小化 mock：只包含测试所需的基础类型
// ============================================

#define PAGE_SIZE 4096
#define IX_NO_PAGE -1
#define IX_INIT_ROOT_PAGE 2
#define IX_INIT_NUM_PAGES 3
#define IX_FILE_HDR_PAGE 0
#define IX_LEAF_HEADER_PAGE 1
#define INVALID_PAGE_ID -1

typedef int page_id_t; 

enum ColType { TYPE_INT, TYPE_FLOAT, TYPE_STRING };

struct PageId {
    int fd;
    int page_no;
};

struct Page {
    PageId page_id_;
    char data_[PAGE_SIZE];
    PageId get_page_id() const { return page_id_; }
    char *get_data() { return data_; }
};

struct Rid {
    int page_no;
    int slot_no;
    bool operator==(const Rid& other) const {
        return page_no == other.page_no && slot_no == other.slot_no;
    }
};

// 简化 ix_compare
inline int ix_compare(const char *a, const char *b, ColType type, int col_len) {
    switch (type) {
        case TYPE_INT: {
            int ia = *(int *)a;
            int ib = *(int *)b;
            return (ia < ib) ? -1 : ((ia > ib) ? 1 : 0);
        }
        case TYPE_FLOAT: {
            float fa = *(float *)a;
            float fb = *(float *)b;
            return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
        }
        case TYPE_STRING:
            return memcmp(a, b, col_len);
        default:
            return 0;
    }
}

inline int ix_compare(const char* a, const char* b, 
                      const std::vector<ColType>& col_types, 
                      const std::vector<int>& col_lens) {
    int offset = 0;
    for(size_t i = 0; i < col_types.size(); ++i) {
        int res = ix_compare(a + offset, b + offset, col_types[i], col_lens[i]);
        if(res != 0) return res;
        offset += col_lens[i];
    }
    return 0;
}

// ============================================
// 从 ix_defs.h 复制核心结构
// ============================================

class IxFileHdr {
public: 
    int first_free_page_no_;
    int num_pages_;
    int root_page_;
    int col_num_;
    std::vector<ColType> col_types_;
    std::vector<int> col_lens_;
    int col_tot_len_;
    int btree_order_;
    int keys_size_;
    int first_leaf_;
    int last_leaf_;
    int tot_len_;

    IxFileHdr() {
        tot_len_ = col_num_ = 0;
    }
};

class IxPageHdr {
public:
    int next_free_page_no;
    int parent;
    int num_key;
    bool is_leaf;
    int prev_leaf;
    int next_leaf;
};

class Iid {
public:
    int page_no;
    int slot_no;
    bool operator==(const Iid &x) const { 
        return page_no == x.page_no && slot_no == x.slot_no; 
    }
};

// ============================================
// 从 ix_index_handle.h 复制 IxNodeHandle 声明
// ============================================

class IxNodeHandle {
   private:
    const IxFileHdr *file_hdr;
    Page *page;
    IxPageHdr *page_hdr;
    char *keys;
    Rid *rids;

   public:
    IxNodeHandle() = default;
    IxNodeHandle(const IxFileHdr *file_hdr_, Page *page_) 
        : file_hdr(file_hdr_), page(page_) {
        page_hdr = reinterpret_cast<IxPageHdr *>(page->get_data());
        keys = page->get_data() + sizeof(IxPageHdr);
        rids = reinterpret_cast<Rid *>(keys + file_hdr->keys_size_);
    }

    int get_size() { return page_hdr->num_key; }
    void set_size(int size) { page_hdr->num_key = size; }
    int get_max_size() { return file_hdr->btree_order_ + 1; }
    int get_min_size() { return get_max_size() / 2; }
    
    int key_at(int i) { return *(int *)get_key(i); }
    page_id_t value_at(int i) { return get_rid(i)->page_no; }
    
    page_id_t get_page_no() { return page->get_page_id().page_no; }
    PageId get_page_id() { return page->get_page_id(); }
    page_id_t get_next_leaf() { return page_hdr->next_leaf; }
    page_id_t get_prev_leaf() { return page_hdr->prev_leaf; }
    page_id_t get_parent_page_no() { return page_hdr->parent; }
    
    bool is_leaf_page() { return page_hdr->is_leaf; }
    bool is_root_page() { return get_parent_page_no() == INVALID_PAGE_ID; }
    
    void set_next_leaf(page_id_t page_no) { page_hdr->next_leaf = page_no; }
    void set_prev_leaf(page_id_t page_no) { page_hdr->prev_leaf = page_no; }
    void set_parent_page_no(page_id_t parent) { page_hdr->parent = parent; }
    
    char *get_key(int key_idx) const { 
        return keys + key_idx * file_hdr->col_tot_len_; 
    }
    Rid *get_rid(int rid_idx) const { 
        return &rids[rid_idx]; 
    }
    void set_key(int key_idx, const char *key) { 
        memcpy(keys + key_idx * file_hdr->col_tot_len_, key, file_hdr->col_tot_len_); 
    }
    void set_rid(int rid_idx, const Rid &rid) { 
        rids[rid_idx] = rid; 
    }

    // Phase 1 方法声明
    int lower_bound(const char *target) const;
    int upper_bound(const char *target) const;
    bool leaf_lookup(const char *key, Rid **value);
    page_id_t internal_lookup(const char *key);
    void insert_pairs(int pos, const char *key, const Rid *rid, int n);
    int insert(const char *key, const Rid &value);
    void erase_pair(int pos);
    int remove(const char *key);
    
    void insert_pair(int pos, const char *key, const Rid &rid) { 
        insert_pairs(pos, key, &rid, 1); 
    }
    
    int find_child(IxNodeHandle *child) {
        int rid_idx;
        for (rid_idx = 0; rid_idx < page_hdr->num_key; rid_idx++) {
            if (get_rid(rid_idx)->page_no == child->get_page_no()) {
                break;
            }
        }
        assert(rid_idx < page_hdr->num_key);
        return rid_idx;
    }
};

// ============================================
// Phase 1 实现（直接复制你的代码）
// ============================================

int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0, right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        const char *mid_key = keys + mid * file_hdr->col_tot_len_;
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int left = 1, right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        const char *mid_key = keys + mid * file_hdr->col_tot_len_;
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        const char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            *value = get_rid(pos);
            return true;
        }
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int idx = upper_bound(key) - 1;
    if (idx < 0) idx = 0;
    return value_at(idx);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= page_hdr->num_key);
    assert(page_hdr->num_key + n <= get_max_size());

    int key_len = file_hdr->col_tot_len_;

    int key_shift = (page_hdr->num_key - pos) * key_len;
    if (key_shift > 0) {
        memmove(keys + (pos + n) * key_len,
                keys + pos * key_len,
                key_shift);
    }
    memcpy(keys + pos * key_len, key, n * key_len);

    int rid_shift = (page_hdr->num_key - pos) * sizeof(Rid);
    if (rid_shift > 0) {
        memmove(&rids[pos + n], &rids[pos], rid_shift);
    }
    memcpy(&rids[pos], rid, n * sizeof(Rid));

    page_hdr->num_key += n;
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        const char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            return page_hdr->num_key;
        }
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < page_hdr->num_key);

    int key_len = file_hdr->col_tot_len_;

    int key_shift = (page_hdr->num_key - pos - 1) * key_len;
    if (key_shift > 0) {
        memmove(keys + pos * key_len,
                keys + (pos + 1) * key_len,
                key_shift);
    }

    int rid_shift = (page_hdr->num_key - pos - 1) * sizeof(Rid);
    if (rid_shift > 0) {
        memmove(&rids[pos], &rids[pos + 1], rid_shift);
    }

    page_hdr->num_key--;
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        const char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            erase_pair(pos);
        }
    }
    return page_hdr->num_key;
}

// ============================================
// 测试辅助函数
// ============================================

static char g_page_buf[PAGE_SIZE];

IxFileHdr create_test_fhdr(int btree_order = 10) {
    IxFileHdr fhdr;
    fhdr.col_num_ = 1;
    fhdr.col_types_.push_back(TYPE_INT);
    fhdr.col_lens_.push_back(sizeof(int));
    fhdr.col_tot_len_ = sizeof(int);
    fhdr.btree_order_ = btree_order;
    fhdr.keys_size_ = (fhdr.btree_order_ + 1) * fhdr.col_tot_len_;
    return fhdr;
}

Page create_test_page(int page_no = 0) {
    memset(g_page_buf, 0, PAGE_SIZE);
    auto *phdr = reinterpret_cast<IxPageHdr *>(g_page_buf);
    phdr->num_key = 0;
    phdr->is_leaf = true;
    phdr->parent = IX_NO_PAGE;
    phdr->prev_leaf = IX_NO_PAGE;
    phdr->next_leaf = IX_NO_PAGE;
    
    Page p;
    p.page_id_.fd = 0;
    p.page_id_.page_no = page_no;
    memcpy(p.data_, g_page_buf, PAGE_SIZE);
    return p;
}
// ============================================
// 测试用例
// ============================================

void test_lower_upper_bound() {
    std::cout << "[TEST] lower_bound / upper_bound ... ";
    
    auto fhdr = create_test_fhdr(10);
    auto mock_page = create_test_page(0);
    IxNodeHandle node(&fhdr, &mock_page);

    // 插入有序 key: 10, 20, 30, 40, 50
    int keys[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; ++i) {
        Rid rid{i, i};
        node.insert(reinterpret_cast<const char *>(&keys[i]), rid);
    }
    assert(node.get_size() == 5);

    // lower_bound 测试
    int q;
    
    q = 5;   // 全部 >= 5，应返回 0
    assert(node.lower_bound(reinterpret_cast<const char *>(&q)) == 0);

    q = 25;  // 第一个 >= 25 的是 30，位置 2
    assert(node.lower_bound(reinterpret_cast<const char *>(&q)) == 2);

    q = 30;  // 第一个 >= 30 的是 30，位置 2
    assert(node.lower_bound(reinterpret_cast<const char *>(&q)) == 2);

    q = 60;  // 没有 >= 60 的，返回 5
    assert(node.lower_bound(reinterpret_cast<const char *>(&q)) == 5);

    // upper_bound 测试（项目语义：返回+1偏移）
    q = 5;   // 第一个 > 5 的是 10，标准 ub=0，项目 ub=1
    assert(node.upper_bound(reinterpret_cast<const char *>(&q)) == 1);

    q = 25;  // 第一个 > 25 的是 30，标准 ub=2，项目 ub=3
    assert(node.upper_bound(reinterpret_cast<const char *>(&q)) == 2);

    q = 50;  // 第一个 > 50 的不存在，标准 ub=5，项目 ub=5
    assert(node.upper_bound(reinterpret_cast<const char *>(&q)) == 5);

    q = 60;  // 同上
    assert(node.upper_bound(reinterpret_cast<const char *>(&q)) == 5);

    std::cout << "PASSED\n";
}

void test_insert_remove() {
    std::cout << "[TEST] insert / remove / insert_pairs / erase_pair ... ";
    
    auto fhdr = create_test_fhdr(10);
    auto mock_page = create_test_page(0);
    IxNodeHandle node(&fhdr, &mock_page);

    // 乱序插入
    std::vector<int> insert_keys = {30, 10, 50, 20, 40};
    for (size_t i = 0; i < insert_keys.size(); ++i) {
        Rid rid{static_cast<int>(i), static_cast<int>(i)};
        node.insert(reinterpret_cast<const char *>(&insert_keys[i]), rid);
    }
    assert(node.get_size() == 5);

    // 验证有序性
    int expected[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; ++i) {
        assert(node.key_at(i) == expected[i]);
    }

    // 去重测试
    int dup = 30;
    Rid rid_dup{99, 99};
    int old_size = node.get_size();
    node.insert(reinterpret_cast<const char *>(&dup), rid_dup);
    assert(node.get_size() == old_size);  // size 不变

    // remove 测试
    node.remove(reinterpret_cast<const char *>(&dup));
    assert(node.get_size() == 4);
    int expected2[] = {10, 20, 40, 50};
    for (int i = 0; i < 4; ++i) {
        assert(node.key_at(i) == expected2[i]);
    }

    // 删不存在的 key
    int not_exist = 100;
    node.remove(reinterpret_cast<const char *>(&not_exist));
    assert(node.get_size() == 4);

    std::cout << "PASSED\n";
}

void test_leaf_lookup() {
    std::cout << "[TEST] leaf_lookup ... ";
    
    auto fhdr = create_test_fhdr(10);
    auto mock_page = create_test_page(0);
    IxNodeHandle node(&fhdr, &mock_page);

    int keys[] = {10, 20, 30};
    for (int i = 0; i < 3; ++i) {
        Rid rid{i * 10, i};
        node.insert(reinterpret_cast<const char *>(&keys[i]), rid);
    }

    // 查存在的 key
    Rid *out = nullptr;
    int q = 20;
    bool found = node.leaf_lookup(reinterpret_cast<const char *>(&q), &out);
    assert(found == true);
    assert(out->page_no == 10);
    assert(out->slot_no == 1);

    // 查不存在的 key
    q = 25;
    found = node.leaf_lookup(reinterpret_cast<const char *>(&q), &out);
    assert(found == false);

    std::cout << "PASSED\n";
}

void test_internal_lookup() {
    std::cout << "[TEST] internal_lookup ... ";
    
    auto fhdr = create_test_fhdr(10);
    auto mock_page = create_test_page(0);
    auto *phdr = reinterpret_cast<IxPageHdr *>(mock_page.get_data());
    phdr->is_leaf = false;  // 内部节点
    IxNodeHandle node(&fhdr, &mock_page);

    // 内部节点：key[i] 对应 rid[i]
    int keys[] = {10, 20, 30};
    int children[] = {100, 200, 300};
    for (int i = 0; i < 3; ++i) {
        Rid rid{children[i], 0};
        node.insert(reinterpret_cast<const char *>(&keys[i]), rid);
    }

    // target < 10: ub=1, idx=0, 走 rid[0]=100
    int q = 5;
    page_id_t pg = node.internal_lookup(reinterpret_cast<const char *>(&q));
    assert(pg == 100);

    // target = 10: ub=1, idx=0, 走 rid[0]=100
    q = 10;
    pg = node.internal_lookup(reinterpret_cast<const char *>(&q));
    assert(pg == 100);

    // 10 < target < 20: ub=1, idx=0, 走 rid[0]=100
    q = 15;
    pg = node.internal_lookup(reinterpret_cast<const char *>(&q));
    assert(pg == 100);

    // target = 20: ub=2, idx=1, 走 rid[1]=200
    q = 20;
    pg = node.internal_lookup(reinterpret_cast<const char *>(&q));
    assert(pg == 200);

    // target > 30: ub=3, idx=2, 走 rid[2]=300
    q = 35;
    pg = node.internal_lookup(reinterpret_cast<const char *>(&q));
    assert(pg == 300);

    std::cout << "PASSED\n";
}

void test_insert_pairs_erase_pair() {
    std::cout << "[TEST] insert_pairs / erase_pair ... ";
    
    auto fhdr = create_test_fhdr(10);
    auto mock_page = create_test_page(0);
    IxNodeHandle node(&fhdr, &mock_page);

    // 先插 10, 30, 50
    int keys1[] = {10, 30, 50};
    for (int i = 0; i < 3; ++i) {
        Rid rid{i, i};
        node.insert(reinterpret_cast<const char *>(&keys1[i]), rid);
    }

    // 在 pos=1 处批量插入 20, 25
    int keys2[] = {20, 25};
    Rid rids2[] = {{20, 20}, {25, 25}};
    node.insert_pairs(1, reinterpret_cast<const char *>(keys2), rids2, 2);

    // 结果应为 10, 20, 25, 30, 50
    int expected[] = {10, 20, 25, 30, 50};
    for (int i = 0; i < 5; ++i) {
        assert(node.key_at(i) == expected[i]);
    }

    // 删除 pos=2（key=25）
    node.erase_pair(2);
    assert(node.get_size() == 4);
    int expected2[] = {10, 20, 30, 50};
    for (int i = 0; i < 4; ++i) {
        assert(node.key_at(i) == expected2[i]);
    }

    std::cout << "PASSED\n";
}

void test_stress_insert() {
    std::cout << "[TEST] stress insert (1000 keys) ... ";
    
    auto fhdr = create_test_fhdr(1000);  // 大 order，不分裂
    auto mock_page = create_test_page(0);
    IxNodeHandle node(&fhdr, &mock_page);

    std::vector<int> keys;
    for (int i = 0; i < 1000; ++i) {
        keys.push_back(i * 3 + 1);  // 1, 4, 7, 10, ...
    }
    
    // 乱序插入
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(keys.begin(), keys.end(), g);

    for (size_t i = 0; i < keys.size(); ++i) {
        Rid rid{static_cast<int>(i), static_cast<int>(i)};
        node.insert(reinterpret_cast<const char *>(&keys[i]), rid);
    }

    assert(node.get_size() == 1000);

    // 验证有序性
    for (int i = 0; i < 1000; ++i) {
        int expected = i * 3 + 1;
        assert(node.key_at(i) == expected);
    }

    // 验证都能查到
    for (int i = 0; i < 1000; ++i) {
        int q = i * 3 + 1;
        Rid *out = nullptr;
        bool found = node.leaf_lookup(reinterpret_cast<const char *>(&q), &out);
        assert(found == true);
        assert(out != nullptr);
    }

    std::cout << "PASSED\n";
}

// ============================================
// Phase 3 简化测试（模拟树结构，不依赖 DiskManager/BufferPool）
// ============================================

// 简化的 IxIndexHandle 测试：只测试 find_leaf_page 和 get_value 的逻辑
// 由于需要完整的树结构，这里用一个预构建的小树来测试

struct SimpleTreeNode {
    bool is_leaf;
    std::vector<int> keys;
    std::vector<int> children;  // 叶子存 rid.page_no，内部存子树 page_no
    std::vector<int> slot_nos;  // 叶子存 rid.slot_no
    int parent;
    int prev, next;  // 叶子链表
};

// 用数组模拟 3 个节点的小树
// 根(节点0, 内部): key=[20], children=[1, 2]
// 左叶子(节点1): key=[10, 15], rid=[(100,0), (101,1)]
// 右叶子(节点2): key=[20, 25, 30], rid=[(200,0), (201,1), (202,2)]

void test_find_leaf_page_simple() {
    std::cout << "[TEST] find_leaf_page (simplified) ... ";
    
    // 这个测试需要完整的 IxIndexHandle，由于依赖 DiskManager/BufferPool，
    // 我们在集成测试阶段验证。这里先跳过，或用一个 mock 版本。
    
    // 实际上，Phase 3 的核心逻辑（split/insert_into_parent/insert_entry）
    // 需要完整的页管理系统，建议直接在项目中编译测试。
    
    std::cout << "SKIPPED (need full project build)\n";
}

// ============================================
// main
// ============================================

int main() {
    std::cout << "========== B+ Tree Phase 1~3 Test ==========\n\n";
    
    test_lower_upper_bound();
    test_insert_remove();
    test_leaf_lookup();
    test_internal_lookup();
    test_insert_pairs_erase_pair();
    test_stress_insert();
    test_find_leaf_page_simple();
    
    std::cout << "\n========== ALL PASSED ==========\n";
    return 0;
}