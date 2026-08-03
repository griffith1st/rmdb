/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <iomanip>
#include <sstream>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_utils.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"
#include "common/datetime_utils.h"

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | BIGINT | DATETIME | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }  
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }     
            default:
                throw InternalError("Unexpected field type");
                break;                        
        }

    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            Context *context) {
    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.agg_type != AGG_NONE && !sel_col.alias.empty() ? sel_col.alias : sel_col.col_name);
    }

    // Print header into buffer
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for(int i = 0; i < captions.size(); ++i) {
        outfile << " " << captions[i] << " |";
    }
    outfile << "\n";

    auto value_to_string = [](const ColMeta &col, const char *rec_buf) {
        std::string col_str;
        if (col.type == TYPE_INT) {
            col_str = std::to_string(*(int *)rec_buf);
        } else if (col.type == TYPE_BIGINT) {
            col_str = std::to_string(*(int64_t *)rec_buf);
        } else if (col.type == TYPE_DATETIME) {
            col_str = datetime_to_string(*(int64_t *)rec_buf);
        } else if (col.type == TYPE_FLOAT) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(6) << *(float *)rec_buf;
            col_str = os.str();
        } else if (col.type == TYPE_STRING) {
            col_str = std::string((char *)rec_buf, col.len);
            col_str.resize(strlen(col_str.c_str()));
        }
        return col_str;
    };

    bool is_aggregate = !sel_cols.empty() && sel_cols[0].agg_type != AGG_NONE;

    if (is_aggregate) {
        struct AggState {
            bool initialized = false;
            int64_t count = 0;
            int64_t sum_int = 0;
            double sum_float = 0;
            std::vector<char> raw;
        };
        std::vector<AggState> states(sel_cols.size());
        for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
            auto tuple = executorTreeRoot->Next();
            auto &cols = executorTreeRoot->cols();
            for (size_t i = 0; i < sel_cols.size(); ++i) {
                auto &state = states[i];
                auto &col = cols[i];
                char *rec_buf = tuple->data + col.offset;
                if (sel_cols[i].agg_type == AGG_COUNT) {
                    state.count++;
                } else if (sel_cols[i].agg_type == AGG_SUM) {
                    if (col.type == TYPE_INT) {
                        state.sum_int += *(int *)rec_buf;
                    } else if (col.type == TYPE_FLOAT) {
                        state.sum_float += *(float *)rec_buf;
                    }
                    state.initialized = true;
                } else if (sel_cols[i].agg_type == AGG_MAX || sel_cols[i].agg_type == AGG_MIN) {
                    if (!state.initialized) {
                        state.raw.assign(rec_buf, rec_buf + col.len);
                        state.initialized = true;
                    } else {
                        int cmp = compare_raw_value(rec_buf, state.raw.data(), col.type, col.len);
                        if ((sel_cols[i].agg_type == AGG_MAX && cmp > 0) ||
                            (sel_cols[i].agg_type == AGG_MIN && cmp < 0)) {
                            state.raw.assign(rec_buf, rec_buf + col.len);
                        }
                    }
                }
            }
        }

        std::vector<std::string> columns;
        auto &cols = executorTreeRoot->cols();
        for (size_t i = 0; i < sel_cols.size(); ++i) {
            if (sel_cols[i].agg_type == AGG_COUNT) {
                columns.push_back(std::to_string(states[i].count));
            } else if (sel_cols[i].agg_type == AGG_SUM) {
                if (cols[i].type == TYPE_INT) {
                    columns.push_back(std::to_string(states[i].sum_int));
                } else {
                    std::ostringstream os;
                    os << std::fixed << std::setprecision(6) << states[i].sum_float;
                    columns.push_back(os.str());
                }
            } else {
                columns.push_back(states[i].initialized ? value_to_string(cols[i], states[i].raw.data()) : "");
            }
        }
        rec_printer.print_record(columns, context);
        outfile << "|";
        for (auto &column : columns) {
            outfile << " " << column << " |";
        }
        outfile << "\n";
        outfile.close();
        rec_printer.print_separator(context);
        RecordPrinter::print_record_count(1, context);
        return;
    }

    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            char *rec_buf = Tuple->data + col.offset;
            col_str = value_to_string(col, rec_buf);
            columns.push_back(col_str);
        }
        // print record into buffer
        rec_printer.print_record(columns, context);
        // print record into file
        outfile << "|";
        for(int i = 0; i < columns.size(); ++i) {
            outfile << " " << columns[i] << " |";
        }
        outfile << "\n";
        num_rec++;
    }
    outfile.close();
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
