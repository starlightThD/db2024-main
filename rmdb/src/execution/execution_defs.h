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

#include <vector>

#include "defs.h"
#include "errors.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"
#include "transaction/txn_defs.h"

inline std::vector<char> make_index_key(const IndexMeta &index, const RmRecord &rec) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (auto &col : index.cols) {
        memcpy(key.data() + offset, rec.data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

struct IndexGapRange {
    std::vector<char> lower_key;
    std::vector<char> upper_key;
    bool lower_inf{true};
    bool upper_inf{true};
    bool lower_closed{false};
    bool upper_closed{false};
};

inline std::vector<ColType> make_index_col_types(const IndexMeta &index) {
    std::vector<ColType> col_types;
    col_types.reserve(index.cols.size());
    for (auto &col : index.cols) {
        col_types.push_back(col.type);
    }
    return col_types;
}

inline std::vector<int> make_index_col_lens(const IndexMeta &index) {
    std::vector<int> col_lens;
    col_lens.reserve(index.cols.size());
    for (auto &col : index.cols) {
        col_lens.push_back(col.len);
    }
    return col_lens;
}

inline LockDataId make_gap_lock_data_id(int index_fd, const IndexMeta &index, const IndexGapRange &range) {
    return LockDataId(index_fd, range.lower_key, range.lower_inf, range.lower_closed, range.upper_key,
                      range.upper_inf, range.upper_closed, make_index_col_types(index), make_index_col_lens(index),
                      LockDataType::GAP);
}

inline LockDataId make_point_gap_lock_data_id(int index_fd, const IndexMeta &index, const std::vector<char> &key) {
    IndexGapRange range;
    range.lower_key = key;
    range.upper_key = key;
    range.lower_inf = false;
    range.upper_inf = false;
    range.lower_closed = true;
    range.upper_closed = true;
    return make_gap_lock_data_id(index_fd, index, range);
}

enum class ScanLockMode {
    READ,
    WRITE
};
