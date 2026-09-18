/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <netinet/in.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <set>
#include <shared_mutex>
#include <thread>

#include "errors.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 8

static volatile sig_atomic_t should_exit = 0;

// 构建全局所需的管理器对象
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
auto ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
auto sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
auto lock_manager = std::make_unique<LockManager>();
auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
auto ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get());
auto log_manager = std::make_unique<LogManager>(disk_manager.get());
auto recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get());
auto planner = std::make_unique<Planner>(sm_manager.get());
auto optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
auto portal = std::make_unique<Portal>(sm_manager.get());
auto analyze = std::make_unique<Analyze>(sm_manager.get());
std::mutex buffer_mutex;
std::shared_mutex catalog_mutex;
std::mutex clients_mutex;
std::set<int> active_clients;

void sigint_handler(int signo) {
    (void)signo;
    // Only async-signal-safe work belongs here. The accept loop performs shutdown.
    should_exit = 1;
}

bool receive_request(int fd, std::string &pending, std::string &request) {
    for (;;) {
        size_t end = pending.find('\0');
        if (end != std::string::npos) {
            request = pending.substr(0, end);
            pending.erase(0, end + 1);
            return true;
        }
        if (pending.size() >= BUFFER_LENGTH) {
            const char error[] = "Error: SQL request exceeds buffer limit\n";
            send(fd, error, sizeof(error), MSG_NOSIGNAL);
            return false;
        }
        char chunk[BUFFER_LENGTH];
        ssize_t count = recv(fd, chunk, BUFFER_LENGTH - pending.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        pending.append(chunk, static_cast<size_t>(count));
    }
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context) {
    context->txn_ = txn_manager->get_transaction(*txn_id);
    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_);
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
    }
}

void client_handler(int fd) {
    std::string pending, request;
    // 需要返回给客户端的结果
    auto response = std::make_unique<char[]>(BUFFER_LENGTH);
    char *data_send = response.get();
    // 需要返回给客户端的结果的长度
    int offset = 0;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;

    std::string output = "establish client connection, sockfd: " + std::to_string(fd) + "\n";
    std::cout << output;

    try {
    while (receive_request(fd, pending, request)) {
        const char *data_recv = request.c_str();

        if (strcmp(data_recv, "exit") == 0) {
            std::cout << "Client exit." << std::endl;
            break;
        }
        if (strcmp(data_recv, "crash") == 0) {
            std::cout << "Server crash" << std::endl;
            exit(1);
        }

        std::cout << "Read from client " << fd << ": " << data_recv << std::endl;

        memset(data_send, '\0', BUFFER_LENGTH);
        offset = 0;

        // 开启事务，初始化系统所需的上下文信息（包括事务对象指针、锁管理器指针、日志管理器指针、存放结果的buffer、记录结果长度的变量）
        Context statement_context(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
        Context *context = &statement_context;
        SetTransaction(&txn_id, context);

        std::shared_lock<std::shared_mutex> catalog_read(catalog_mutex, std::defer_lock);
        std::unique_lock<std::shared_mutex> catalog_write(catalog_mutex, std::defer_lock);
        std::unique_lock<std::mutex> parser_guard(buffer_mutex);
        std::unique_ptr<yy_buffer_state, decltype(&yy_delete_buffer)> buf(yy_scan_string(data_recv), yy_delete_buffer);
        try {
            ast::parse_tree.reset();
            if (yyparse() != 0) throw InternalError("SQL syntax error");
            if (ast::parse_tree != nullptr) {
                bool ddl = std::dynamic_pointer_cast<ast::CreateTable>(ast::parse_tree) ||
                           std::dynamic_pointer_cast<ast::DropTable>(ast::parse_tree) ||
                           std::dynamic_pointer_cast<ast::CreateIndex>(ast::parse_tree) ||
                           std::dynamic_pointer_cast<ast::DropIndex>(ast::parse_tree);
                if (ddl) catalog_write.lock();
                else catalog_read.lock();
                std::shared_ptr<Query> query = analyze->do_analyze(ast::parse_tree);
                buf.reset();
                parser_guard.unlock();
                std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
                std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
                portal->run(portalStmt, ql_manager.get(), &txn_id, context);
                portal->drop();
            }
        } catch (TransactionAbortException &e) {
            std::string message = "abort\n";
            memcpy(data_send, message.c_str(), message.length());
            offset = message.length();
            data_send[offset] = '\0';
            txn_manager->abort(context->txn_, log_manager.get());
            std::cout << e.GetInfo() << std::endl;
            std::ofstream outfile("output.txt", std::ios::app);
            outfile << message;
        } catch (std::exception &e) {
            if (!catalog_read.owns_lock() && !catalog_write.owns_lock()) catalog_read.lock();
            // Includes semantic errors and numeric conversion errors raised by
            // the parser. Errors abort the entire current transaction.
            std::string message = e.what();
            if (message.rfind("Error:", 0) != 0) message = "Error: " + message;
            message += "\n";
            offset = std::min(static_cast<int>(message.size()), BUFFER_LENGTH - 1);
            memcpy(data_send, message.data(), offset);
            data_send[offset] = '\0';
            txn_manager->abort(context->txn_, context->log_mgr_);
            std::cerr << message;
            std::ofstream outfile("output.txt", std::ios::app);
            outfile << "failure\n";
        }
        if (parser_guard.owns_lock()) {
            buf.reset();
            parser_guard.unlock();
        }
        // Persist an implicit commit before the client receives success.
        if(context->txn_->get_txn_mode() == false &&
           context->txn_->get_state() != TransactionState::COMMITTED &&
           context->txn_->get_state() != TransactionState::ABORTED)
        {
            txn_manager->commit(context->txn_, context->log_mgr_);
        }
        if (context->txn_->get_state() == TransactionState::COMMITTED ||
            context->txn_->get_state() == TransactionState::ABORTED) {
            delete context->txn_;
            context->txn_ = nullptr;
            txn_id = INVALID_TXN_ID;
        }
        if (catalog_read.owns_lock()) catalog_read.unlock();
        if (catalog_write.owns_lock()) catalog_write.unlock();
        size_t sent = 0;
        while (sent < static_cast<size_t>(offset + 1)) {
            ssize_t count = send(fd, data_send + sent, offset + 1 - sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            sent += count;
        }
        if (sent != static_cast<size_t>(offset + 1)) break;
    }
    } catch (const std::exception &e) {
        std::cerr << "Connection failure: " << e.what() << '\n';
    }
    std::shared_lock<std::shared_mutex> catalog_read(catalog_mutex);
    if (auto *txn = txn_manager->get_transaction(txn_id)) {
        txn_manager->abort(txn, log_manager.get());
        delete txn;
    }
    std::cout << "Terminating current client_connection..." << std::endl;
}

void start_server() {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Worker> workers;
    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    assert(sockfd_server != -1);
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        std::cout << "Bind error!" << std::endl;
        exit(1);
    }

    fd_temp = listen(sockfd_server, MAX_CONN_LIMIT);
    if (fd_temp == -1) {
        std::cout << "Listen error!" << std::endl;
        exit(1);
    }

    while (!should_exit) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->finished->load()) {
                it->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
        struct sockaddr_in s_addr_client {};
        int client_length = sizeof(s_addr_client);

        pollfd listener{sockfd_server, POLLIN, 0};
        int ready = poll(&listener, 1, 100);
        if (ready < 0 && errno != EINTR) throw UnixError();
        if (should_exit) break;
        if (ready <= 0) continue;
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            std::cout << "Accept error!" << std::endl;
            continue;  // ignore current socket ,continue while loop.
        }
        
        {
            std::lock_guard<std::mutex> guard(clients_mutex);
            active_clients.insert(sockfd);
        }
        auto finished = std::make_shared<std::atomic<bool>>(false);
        try {
            workers.push_back({std::thread([sockfd, finished] {
                try {
                    client_handler(sockfd);
                } catch (const std::exception &e) {
                    std::cerr << "Connection cleanup failed: " << e.what() << '\n';
                }
                {
                    std::lock_guard<std::mutex> guard(clients_mutex);
                    active_clients.erase(sockfd);
                    close(sockfd);
                }
                *finished = true;
            }), finished});
        } catch (const std::system_error &e) {
            std::lock_guard<std::mutex> guard(clients_mutex);
            active_clients.erase(sockfd);
            close(sockfd);
            std::cerr << "Cannot start client worker: " << e.what() << '\n';
        }

    }

    // Clear
    std::cout << " Try to close all client-connection.\n";
    close(sockfd_server);
    {
        std::lock_guard<std::mutex> guard(clients_mutex);
        for (int client : active_clients) shutdown(client, SHUT_RDWR);
    }
    for (auto &worker : workers) worker.thread.join();
    log_manager->flush_log_to_disk();
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    try {
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);

        // recovery database
        recovery->analyze();
        log_manager->set_next_lsn(recovery->next_lsn());
        txn_manager->set_next_txn_id(recovery->next_txn_id());
        recovery->redo();
        recovery->undo();
        
        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return 0;
}
