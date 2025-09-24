// tcp_server.cpp
// TCP服务器程序 - 支持多客户端连接、文本消息和文件传输（含断点续传）
// 改进版本：
// 1. 与客户端消息格式保持一致（[总长度][消息头][数据]）
// 2. 添加网络字节序转换
// 3. 修复端口默认值问题
// 4. 增强错误处理和日志记录

#include <iostream>      // 标准输入输出流
#include <fstream>       // 文件流操作
#include <string>        // 字符串处理
#include <vector>        // 动态数组容器
#include <cstring>       // C字符串操作
#include <thread>        // 多线程支持
#include <mutex>         // 互斥锁，用于线程同步
#include <map>           // 映射容器，用于管理文件传输状态
#include <chrono>        // 时间处理
#include <signal.h>      // 信号处理（如Ctrl+C）
#include <sys/socket.h>  // socket编程基础函数
#include <netinet/in.h>  // Internet地址族
#include <arpa/inet.h>   // IP地址转换函数
#include <unistd.h>      // Unix标准函数
#include <sys/stat.h>    // 文件状态检查
#include <fcntl.h>       // 文件控制（如非阻塞模式）
#include <iomanip>       // IO流格式控制

using namespace std;
using namespace chrono;

// ========== 全局变量 ==========
bool server_running = true;  // 服务器运行标志
mutex cout_mutex;            // 用于保护cout输出的互斥锁，防止多线程输出混乱
mutex file_mutex;            // 用于保护文件操作的互斥锁

// ========== 消息类型枚举 ==========
// 必须与客户端保持一致
enum MessageType {
    TEXT_MSG = 1,      // 文本消息
    FILE_START = 2,    // 文件传输开始
    FILE_CHUNK = 3,    // 文件数据块
    FILE_END = 4,      // 文件传输结束
    FILE_RESUME = 5    // 断点续传请求
};

// ========== 消息头结构体 ==========
// 必须与客户端保持完全一致
struct MessageHeader {
    uint32_t type;           // 消息类型
    uint32_t data_length;    // 数据长度（不包括消息头）
    uint32_t chunk_id;       // 文件块编号
    uint32_t total_chunks;   // 总块数
    uint64_t file_size;      // 文件总大小
    char filename[256];      // 文件名
};

// ========== 文件传输状态结构体 ==========
// 用于跟踪每个文件的传输状态
struct FileTransferState {
    string filename;                           // 文件名
    size_t total_size;                        // 文件总大小
    size_t received_size;                     // 已接收大小
    map<uint32_t, bool> received_chunks;      // 已接收块的记录
    ofstream file;                             // 文件输出流
    high_resolution_clock::time_point start_time;  // 开始时间
    mutex state_mutex;                        // 保护状态的互斥锁
};

/**
 * 信号处理函数
 * 用于优雅地关闭服务器（响应Ctrl+C）
 * @param sig 信号编号
 */
void signal_handler(int sig) {
    if (sig == SIGINT) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "\n收到中断信号，服务器正在关闭..." << endl;
        server_running = false;
    }
}

/**
 * 改进的发送消息函数
 * 将消息头和数据打包后一起发送
 * 消息格式：[总长度(4字节)][消息头][数据]
 * @param socket 套接字描述符
 * @param header 消息头
 * @param data 消息数据（可选）
 * @return 发送成功返回true，失败返回false
 */
bool send_message(int socket, const MessageHeader& header, const char* data = nullptr) {
    // 计算消息总长度
    uint32_t total_length = sizeof(MessageHeader);
    if (data && header.data_length > 0) {
        total_length += header.data_length;
    }
    
    // 创建发送缓冲区
    vector<char> send_buffer(sizeof(uint32_t) + total_length);
    
    // 1. 写入总长度（网络字节序）
    uint32_t net_length = htonl(total_length);
    memcpy(send_buffer.data(), &net_length, sizeof(uint32_t));
    
    // 2. 写入消息头（转换字节序）
    MessageHeader net_header = header;
    net_header.type = htonl(header.type);
    net_header.data_length = htonl(header.data_length);
    net_header.chunk_id = htonl(header.chunk_id);
    net_header.total_chunks = htonl(header.total_chunks);
    // file_size是64位，这里简化处理
    memcpy(send_buffer.data() + sizeof(uint32_t), &net_header, sizeof(MessageHeader));
    
    // 3. 写入数据
    if (data && header.data_length > 0) {
        memcpy(send_buffer.data() + sizeof(uint32_t) + sizeof(MessageHeader), 
               data, header.data_length);
    }
    
    // 一次性发送整个缓冲区
    size_t total_to_send = send_buffer.size();
    size_t total_sent = 0;
    
    while (total_sent < total_to_send) {
        ssize_t sent = send(socket, send_buffer.data() + total_sent, 
                           total_to_send - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EPIPE) {
                // 连接已断开
                return false;
            }
            lock_guard<mutex> lock(cout_mutex);
            cerr << "发送数据失败: " << strerror(errno) << endl;
            return false;
        }
        total_sent += sent;
    }
    
    return true;
}

/**
 * 改进的接收消息函数
 * 先接收总长度，然后接收完整消息
 * @param socket 套接字描述符
 * @param header 消息头（输出参数）
 * @param data 消息数据缓冲区（输出参数）
 * @return 接收成功返回true，失败返回false
 */
bool receive_message(int socket, MessageHeader& header, vector<char>& data) {
    // 1. 先接收消息总长度
    uint32_t net_length;
    ssize_t received = recv(socket, &net_length, sizeof(uint32_t), MSG_WAITALL);
    if (received <= 0) {
        return false;  // 连接断开或错误
    }
    
    uint32_t total_length = ntohl(net_length);  // 转换为主机字节序
    
    // 验证长度合理性
    if (total_length < sizeof(MessageHeader) || total_length > 10 * 1024 * 1024) {
        lock_guard<mutex> lock(cout_mutex);
        cerr << "接收到无效的消息长度: " << total_length << endl;
        return false;
    }
    
    // 2. 接收完整消息
    vector<char> recv_buffer(total_length);
    size_t total_received = 0;
    
    while (total_received < total_length) {
        ssize_t rcvd = recv(socket, recv_buffer.data() + total_received, 
                           total_length - total_received, 0);
        if (rcvd <= 0) {
            return false;
        }
        total_received += rcvd;
    }
    
    // 3. 解析消息头
    MessageHeader net_header;
    memcpy(&net_header, recv_buffer.data(), sizeof(MessageHeader));
    
    // 转换字节序
    header.type = ntohl(net_header.type);
    header.data_length = ntohl(net_header.data_length);
    header.chunk_id = ntohl(net_header.chunk_id);
    header.total_chunks = ntohl(net_header.total_chunks);
    header.file_size = net_header.file_size;  // 简化处理
    strcpy(header.filename, net_header.filename);
    
    // 4. 提取数据部分
    if (header.data_length > 0) {
        data.resize(header.data_length);
        memcpy(data.data(), recv_buffer.data() + sizeof(MessageHeader), header.data_length);
    } else {
        data.clear();
    }
    
    return true;
}

/**
 * 处理客户端连接的线程函数
 * 每个客户端连接都在独立的线程中处理
 * @param client_socket 客户端套接字
 * @param client_addr 客户端地址信息
 */
void handle_client(int client_socket, sockaddr_in client_addr) {
    // 获取客户端IP地址
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);
    
    // 记录客户端连接
    {
        lock_guard<mutex> lock(cout_mutex);
        cout << "\n[新连接] 客户端: " << client_ip << ":" << client_port << endl;
        cout << "========================================" << endl;
    }
    
    // 为该客户端维护的文件传输状态映射
    map<string, FileTransferState> file_transfers;
    
    // 消息接收缓冲区
    MessageHeader header;
    vector<char> data;
    
    // 主循环：持续处理客户端消息
    while (server_running) {
        // 接收消息
        if (!receive_message(client_socket, header, data)) {
            break;  // 连接断开
        }
        
        // 根据消息类型处理
        switch (header.type) {
            case TEXT_MSG: {
                // ========== 处理文本消息 ==========
                string message(data.begin(), data.end());
                
                // 检查是否是断开连接消息
                if (message == "CLIENT_DISCONNECT") {
                    lock_guard<mutex> lock(cout_mutex);
                    cout << "[" << client_ip << "] 客户端主动断开连接" << endl;
                    break;
                }
                
                // 显示接收到的消息
                {
                    lock_guard<mutex> lock(cout_mutex);
                    cout << "[" << client_ip << "] 文本消息: " << message << endl;
                }
                
                // 构造并发送回复
                string reply = "服务器已收到: " + message;
                MessageHeader reply_header = {0};
                reply_header.type = TEXT_MSG;
                reply_header.data_length = reply.length();
                
                if (!send_message(client_socket, reply_header, reply.c_str())) {
                    lock_guard<mutex> lock(cout_mutex);
                    cerr << "[" << client_ip << "] 发送回复失败" << endl;
                }
                break;
            }
            
            case FILE_START: {
                // ========== 开始文件传输 ==========
                string filename(header.filename);
                
                {
                    lock_guard<mutex> lock(cout_mutex);
                    cout << "\n[" << client_ip << "] 开始接收文件: " << filename << endl;
                    cout << "  文件大小: " << header.file_size / (1024.0 * 1024.0) << " MB" << endl;
                    cout << "  总块数: " << header.total_chunks << endl;
                }
                
                // 创建或获取文件传输状态
                auto& state = file_transfers[filename];
                state.filename = filename;
                state.total_size = header.file_size;
                state.start_time = high_resolution_clock::now();
                
                // 检查是否支持断点续传
                string save_path = "received_" + filename;
                struct stat file_stat;
                
                if (stat(save_path.c_str(), &file_stat) == 0) {
                    // 文件已存在，计算已接收的块数
                    state.received_size = file_stat.st_size;
                    uint32_t resume_chunk = state.received_size / 4096;  // 假设每块4KB
                    
                    // 打开文件（追加模式）
                    state.file.open(save_path, ios::binary | ios::app);
                    
                    // 发送断点续传请求
                    MessageHeader resume_header = {0};
                    resume_header.type = FILE_RESUME;
                    resume_header.chunk_id = resume_chunk;
                    resume_header.data_length = 0;
                    strcpy(resume_header.filename, filename.c_str());
                    
                    send_message(client_socket, resume_header);
                    
                    {
                        lock_guard<mutex> lock(cout_mutex);
                        cout << "  [断点续传] 从块 " << resume_chunk << " 继续接收" << endl;
                    }
                } else {
                    // 新文件
                    state.received_size = 0;
                    state.file.open(save_path, ios::binary);
                    
                    if (!state.file.is_open()) {
                        lock_guard<mutex> lock(cout_mutex);
                        cerr << "  [错误] 无法创建文件: " << save_path << endl;
                    }
                }
                break;
            }
            
            case FILE_CHUNK: {
                // ========== 接收文件块 ==========
                string filename(header.filename);
                auto it = file_transfers.find(filename);
                
                if (it == file_transfers.end()) {
                    lock_guard<mutex> lock(cout_mutex);
                    cerr << "[" << client_ip << "] 错误：收到未知文件的数据块" << endl;
                    break;
                }
                
                auto& state = it->second;
                
                // 写入文件
                if (state.file.is_open()) {
                    state.file.write(data.data(), data.size());
                    state.file.flush();  // 确保数据写入磁盘
                }
                
                // 更新状态
                state.received_size += data.size();
                state.received_chunks[header.chunk_id] = true;
                
                // 显示进度（每10个块更新一次显示）
                if (header.chunk_id % 10 == 0 || header.chunk_id == header.total_chunks - 1) {
                    float progress = (float)(header.chunk_id + 1) / header.total_chunks * 100;
                    lock_guard<mutex> lock(cout_mutex);
                    cout << "\r[" << client_ip << "] " << filename 
                         << " 进度: " << fixed << setprecision(2) << progress << "% "
                         << "(" << header.chunk_id + 1 << "/" << header.total_chunks << " 块)" 
                         << flush;
                }
                break;
            }
            
            case FILE_END: {
                // ========== 文件传输结束 ==========
                string filename(header.filename);
                auto it = file_transfers.find(filename);
                
                if (it != file_transfers.end()) {
                    auto& state = it->second;
                    
                    // 关闭文件
                    if (state.file.is_open()) {
                        state.file.close();
                    }
                    
                    // 计算传输统计信息
                    auto end_time = high_resolution_clock::now();
                    auto duration = duration_cast<milliseconds>(end_time - state.start_time);
                    double seconds = duration.count() / 1000.0;
                    double speed_mbps = (state.received_size / (1024.0 * 1024.0)) / seconds;
                    
                    {
                        lock_guard<mutex> lock(cout_mutex);
                        cout << "\n[" << client_ip << "] 文件接收完成: " << filename << endl;
                        cout << "  保存为: received_" << filename << endl;
                        cout << "  文件大小: " << state.received_size / (1024.0 * 1024.0) << " MB" << endl;
                        cout << "  传输时间: " << seconds << " 秒" << endl;
                        cout << "  传输速度: " << fixed << setprecision(2) << speed_mbps << " MB/s" << endl;
                        cout << "========================================" << endl;
                    }
                    
                    // 清理传输状态
                    file_transfers.erase(it);
                }
                break;
            }
            
            default: {
                // ========== 未知消息类型 ==========
                lock_guard<mutex> lock(cout_mutex);
                cerr << "[" << client_ip << "] 收到未知类型消息: " << header.type << endl;
                break;
            }
        }
    }
    
    // 清理未完成的文件传输
    for (auto& pair : file_transfers) {
        if (pair.second.file.is_open()) {
            pair.second.file.close();
        }
    }
    
    // 关闭客户端连接
    close(client_socket);
    
    {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[断开连接] 客户端: " << client_ip << ":" << client_port << endl;
    }
}

/**
 * 向客户端发送文件
 * 支持大文件分块传输
 * @param socket 客户端套接字
 * @param filepath 要发送的文件路径
 */
void send_file_to_client(int socket, const string& filepath) {
    // 打开文件
    ifstream file(filepath, ios::binary | ios::ate);
    if (!file.is_open()) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "无法打开文件: " << filepath << endl;
        return;
    }
    
    // 获取文件信息
    size_t file_size = file.tellg();
    file.seekg(0, ios::beg);
    
    // 提取文件名
    size_t pos = filepath.find_last_of("/\\");
    string filename = (pos == string::npos) ? filepath : filepath.substr(pos + 1);
    
    // 计算分块信息
    const size_t chunk_size = 4096;  // 4KB每块
    uint32_t total_chunks = (file_size + chunk_size - 1) / chunk_size;
    
    {
        lock_guard<mutex> lock(cout_mutex);
        cout << "准备发送文件: " << filename << endl;
        cout << "文件大小: " << file_size / (1024.0 * 1024.0) << " MB" << endl;
        cout << "总块数: " << total_chunks << endl;
    }
    
    // 发送文件开始消息
    MessageHeader start_header = {0};
    start_header.type = FILE_START;
    start_header.data_length = 0;
    start_header.file_size = file_size;
    start_header.total_chunks = total_chunks;
    strcpy(start_header.filename, filename.c_str());
    
    if (!send_message(socket, start_header)) {
        lock_guard<mutex> lock(cout_mutex);
        cerr << "发送文件开始消息失败" << endl;
        file.close();
        return;
    }
    
    // 发送文件块
    vector<char> buffer(chunk_size);
    uint32_t chunk_id = 0;
    auto start_time = high_resolution_clock::now();
    
    while (!file.eof() && chunk_id < total_chunks) {
        file.read(buffer.data(), chunk_size);
        size_t bytes_read = file.gcount();
        
        if (bytes_read > 0) {
            MessageHeader chunk_header = {0};
            chunk_header.type = FILE_CHUNK;
            chunk_header.data_length = bytes_read;
            chunk_header.chunk_id = chunk_id;
            chunk_header.total_chunks = total_chunks;
            strcpy(chunk_header.filename, filename.c_str());
            
            if (!send_message(socket, chunk_header, buffer.data())) {
                lock_guard<mutex> lock(cout_mutex);
                cerr << "发送文件块失败" << endl;
                file.close();
                return;
            }
            
            chunk_id++;
            
            // 显示进度
            if (chunk_id % 10 == 0 || chunk_id == total_chunks) {
                float progress = (float)chunk_id / total_chunks * 100;
                lock_guard<mutex> lock(cout_mutex);
                cout << "\r发送进度: " << fixed << setprecision(2) << progress << "%" << flush;
            }
        }
    }
    
    // 发送文件结束消息
    MessageHeader end_header = {0};
    end_header.type = FILE_END;
    end_header.data_length = 0;
    strcpy(end_header.filename, filename.c_str());
    send_message(socket, end_header);
    
    // 计算传输速度
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;
    double speed_mbps = (file_size / (1024.0 * 1024.0)) / seconds;
    
    {
        lock_guard<mutex> lock(cout_mutex);
        cout << "\n文件发送完成: " << filename << endl;
        cout << "传输速度: " << fixed << setprecision(2) << speed_mbps << " MB/s" << endl;
    }
    
    file.close();
}

/**
 * 主函数 - 服务器入口点
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * 使用方式：
 *   1. 无参数运行：程序会提示输入端口
 *   2. ./tcp_server [port]
 */
int main(int argc, char* argv[]) {
    // ========== 端口配置 ==========
    int port = 8888;  // 默认端口
    
    if (argc == 1) {
        // 交互式输入端口
        cout << "请输入服务器监听端口(默认8888): ";
        string port_input;
        getline(cin, port_input);
        
        if (port_input.empty()) {
            port = 8888;  // 使用默认端口
        } else {
            try {
                port = stoi(port_input);
                if (port < 1 || port > 65535) {
                    cerr << "端口号必须在1-65535范围内，使用默认端口8888" << endl;
                    port = 8888;
                }
            } catch (const exception& e) {
                cerr << "端口号格式错误，使用默认端口8888" << endl;
                port = 8888;
            }
        }
    } else if (argc == 2) {
        // 从命令行参数获取端口
        try {
            port = stoi(argv[1]);
            if (port < 1 || port > 65535) {
                cerr << "端口号必须在1-65535范围内" << endl;
                return 1;
            }
        } catch (const exception& e) {
            cerr << "无效的端口号: " << argv[1] << endl;
            return 1;
        }
    } else {
        cerr << "用法: " << argv[0] << " [端口号]" << endl;
        return 1;
    }
    
    // ========== 设置信号处理 ==========
    // 注册SIGINT（Ctrl+C）信号处理函数
    signal(SIGINT, signal_handler);
    
    // ========== 创建服务器socket ==========
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        cerr << "创建socket失败: " << strerror(errno) << endl;
        return 1;
    }
    
    // ========== 设置socket选项 ==========
    // SO_REUSEADDR: 允许重用处于TIME_WAIT状态的地址
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        cerr << "设置socket选项失败: " << strerror(errno) << endl;
        close(server_socket);
        return 1;
    }
    
    // ========== 绑定地址和端口 ==========
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;              // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // 监听所有网络接口
    server_addr.sin_port = htons(port);            // 端口号（转换为网络字节序）
    
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "绑定端口失败: " << strerror(errno) << endl;
        cerr << "可能原因：端口已被占用或权限不足" << endl;
        close(server_socket);
        return 1;
    }
    
    // ========== 开始监听 ==========
    // 第二个参数是等待队列的最大长度
    if (listen(server_socket, 10) < 0) {
        cerr << "监听失败: " << strerror(errno) << endl;
        close(server_socket);
        return 1;
    }
    
    // 显示服务器信息
    cout << "========================================" << endl;
    cout << "TCP文件传输服务器" << endl;
    cout << "监听端口: " << port << endl;
    cout << "按 Ctrl+C 优雅关闭服务器" << endl;
    cout << "========================================" << endl;
    
    // ========== 设置非阻塞模式 ==========
    // 允许在没有连接时不会阻塞，便于检查server_running标志
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);
    
    // ========== 主循环：接受客户端连接 ==========
    while (server_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // 接受新的客户端连接
        int client_socket = accept(server_socket, (sockaddr*)&client_addr, &client_len);
        
        if (client_socket >= 0) {
            // 成功接受连接，创建新线程处理该客户端
            thread client_thread(handle_client, client_socket, client_addr);
            client_thread.detach();  // 分离线程，使其独立运行
            
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // EAGAIN和EWOULDBLOCK是非阻塞模式下的正常情况
            // 其他错误才需要报告
            lock_guard<mutex> lock(cout_mutex);
            cerr << "接受连接失败: " << strerror(errno) << endl;
        }
        
        // 短暂休眠，减少CPU占用
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    
    // ========== 清理资源 ==========
    close(server_socket);
    
    cout << "\n服务器已安全关闭" << endl;
    cout << "========================================" << endl;
    
    return 0;
}