// tcp_client.cpp
// TCP客户端程序 - 支持文本消息和文件传输（含断点续传功能）
// 改进版本：
// 1. 修复了IP地址验证问题
// 2. 消息头和数据一起发送，避免分包问题
// 3. 添加消息总长度字段，提高传输可靠性
// 4. 去除重复线程创建问题

#include <iostream>      // 标准输入输出流
#include <fstream>       // 文件流操作
#include <string>        // 字符串处理
#include <vector>        // 动态数组容器
#include <cstring>       // C字符串操作（memset, strcpy等）
#include <thread>        // 多线程支持
#include <chrono>        // 时间处理
#include <sys/socket.h>  // socket编程基础函数
#include <netinet/in.h>  // Internet地址族
#include <arpa/inet.h>   // IP地址转换函数
#include <unistd.h>      // Unix标准函数（close等）
#include <sys/stat.h>    // 文件状态检查
#include <sstream>       // 字符串流处理

using namespace std;
using namespace chrono;

// 消息类型枚举定义（必须与服务器端保持一致）
enum MessageType {
    TEXT_MSG = 1,      // 文本消息
    FILE_START = 2,    // 文件传输开始标记
    FILE_CHUNK = 3,    // 文件数据块
    FILE_END = 4,      // 文件传输结束标记
    FILE_RESUME = 5    // 断点续传请求
};

// 消息头结构体定义（必须与服务器端保持一致）
// 每个消息都包含此消息头，用于描述消息的基本信息
struct MessageHeader {
    uint32_t type;           // 消息类型（见MessageType枚举）
    uint32_t data_length;    // 实际数据长度（不包括消息头）
    uint32_t chunk_id;       // 文件块编号（用于文件传输）
    uint32_t total_chunks;   // 文件总块数（用于文件传输）
    uint64_t file_size;      // 文件总大小（字节）
    char filename[256];      // 文件名（最大255字符+结束符）
};

/**
 * 改进的发送消息函数
 * 将消息头和数据打包后一起发送，并在最前面加上总长度
 * 消息格式：[总长度(4字节)][消息头][数据]
 * @param socket 套接字描述符
 * @param header 消息头
 * @param data 消息数据（可选）
 * @return 发送成功返回true，失败返回false
 */
bool send_message(int socket, const MessageHeader& header, const char* data = nullptr) {
    // 计算消息总长度（消息头 + 数据）
    uint32_t total_length = sizeof(MessageHeader);
    if (data && header.data_length > 0) {
        total_length += header.data_length;
    }
    
    // 创建发送缓冲区（4字节长度 + 消息头 + 数据）
    vector<char> send_buffer(sizeof(uint32_t) + total_length);
    
    // 1. 写入总长度（网络字节序）
    uint32_t net_length = htonl(total_length);
    memcpy(send_buffer.data(), &net_length, sizeof(uint32_t));
    
    // 2. 写入消息头（需要转换字节序）
    MessageHeader net_header = header;
    net_header.type = htonl(header.type);
    net_header.data_length = htonl(header.data_length);
    net_header.chunk_id = htonl(header.chunk_id);
    net_header.total_chunks = htonl(header.total_chunks);
    // file_size是64位，需要特殊处理（这里简化处理）
    memcpy(send_buffer.data() + sizeof(uint32_t), &net_header, sizeof(MessageHeader));
    
    // 3. 写入数据（如果有）
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
                cerr << "连接已断开(EPIPE)" << endl;
            } else {
                cerr << "发送数据失败: " << strerror(errno) << endl;
            }
            return false;
        }
        total_sent += sent;
    }
    
    return true;
}

/**
 * 改进的接收消息函数
 * 先接收总长度，然后一次性接收完整消息
 * @param socket 套接字描述符
 * @param header 消息头（输出参数）
 * @param data 消息数据缓冲区（输出参数）
 * @return 接收成功返回true，失败返回false
 */
bool receive_message(int socket, MessageHeader& header, vector<char>& data) {
    // 1. 先接收消息总长度（4字节）
    uint32_t net_length;
    ssize_t received = recv(socket, &net_length, sizeof(uint32_t), MSG_WAITALL);
    if (received <= 0) {
        return false;  // 连接断开或出错
    }
    
    uint32_t total_length = ntohl(net_length);  // 转换为主机字节序
    
    // 验证长度合理性
    if (total_length < sizeof(MessageHeader) || total_length > 10 * 1024 * 1024) {
        cerr << "接收到无效的消息长度: " << total_length << endl;
        return false;
    }
    
    // 2. 接收完整消息（消息头 + 数据）
    vector<char> recv_buffer(total_length);
    size_t total_received = 0;
    
    while (total_received < total_length) {
        ssize_t rcvd = recv(socket, recv_buffer.data() + total_received, 
                           total_length - total_received, 0);
        if (rcvd <= 0) {
            cerr << "接收消息体失败" << endl;
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
    
    // 4. 提取数据部分（如果有）
    if (header.data_length > 0) {
        data.resize(header.data_length);
        memcpy(data.data(), recv_buffer.data() + sizeof(MessageHeader), header.data_length);
    } else {
        data.clear();
    }
    
    return true;
}

/**
 * 发送文本消息
 * @param socket 套接字描述符
 * @param message 要发送的文本消息
 */
void send_text_message(int socket, const string& message) {
    // 构造文本消息头
    MessageHeader header = {0};  
    header.type = TEXT_MSG;      
    header.data_length = message.length();  // 使用data_length字段
    
    // 发送消息
    if (send_message(socket, header, message.c_str())) {
        cout << "文本消息已发送: " << message << endl;
    } else {
        cerr << "发送文本消息失败" << endl;
    }
}

/**
 * 发送文件（支持断点续传）
 * 将文件分块发送，支持大文件传输和断点续传
 * @param socket 套接字描述符
 * @param filepath 要发送的文件路径
 */
void send_file_with_resume(int socket, const string& filepath) {
    // 打开文件
    ifstream file(filepath, ios::binary | ios::ate);
    if (!file.is_open()) {
        cerr << "无法打开文件: " << filepath << endl;
        return;
    }
    
    // 获取文件大小
    size_t file_size = file.tellg();
    file.seekg(0, ios::beg);
    
    // 从完整路径中提取文件名
    size_t pos = filepath.find_last_of("/\\");
    string filename = (pos == string::npos) ? filepath : filepath.substr(pos + 1);
    
    // 计算文件分块信息
    const size_t chunk_size = 4096;  // 每个块4KB（增大以提高效率）
    uint32_t total_chunks = (file_size + chunk_size - 1) / chunk_size;
    
    // 显示文件信息
    cout << "准备发送文件: " << filename << endl;
    cout << "文件大小: " << file_size / (1024.0 * 1024.0) << " MB" << endl;
    cout << "总块数: " << total_chunks << endl;
    
    // 构造并发送文件开始消息
    MessageHeader start_header = {0};
    start_header.type = FILE_START;          
    start_header.data_length = 0;            // 开始消息没有数据部分
    start_header.file_size = file_size;      // 文件总大小
    start_header.total_chunks = total_chunks;
    strcpy(start_header.filename, filename.c_str());
    
    if (!send_message(socket, start_header)) {
        cerr << "发送文件开始消息失败" << endl;
        file.close();
        return;
    }
    
    // 检查服务器是否请求断点续传
    uint32_t start_chunk = 0;
    MessageHeader resume_header;
    vector<char> resume_data;
    
    // 使用select等待服务器响应
    fd_set read_fds;
    struct timeval timeout;
    FD_ZERO(&read_fds);
    FD_SET(socket, &read_fds);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    
    if (select(socket + 1, &read_fds, NULL, NULL, &timeout) > 0) {
        if (receive_message(socket, resume_header, resume_data)) {
            if (resume_header.type == FILE_RESUME) {
                start_chunk = resume_header.chunk_id;
                cout << "服务器请求断点续传，从块 " << start_chunk << " 开始" << endl;
                file.seekg(start_chunk * chunk_size, ios::beg);
            }
        }
    }
    
    // 开始发送文件块
    vector<char> buffer(chunk_size);
    uint32_t chunk_id = start_chunk;
    auto start_time = high_resolution_clock::now();
    size_t total_sent = start_chunk * chunk_size;
    
    // 循环读取并发送文件块
    while (!file.eof() && chunk_id < total_chunks) {
        file.read(buffer.data(), chunk_size);
        size_t bytes_read = file.gcount();
        
        if (bytes_read > 0) {
            // 构造文件块消息头
            MessageHeader chunk_header = {0};
            chunk_header.type = FILE_CHUNK;
            chunk_header.data_length = bytes_read;    // 实际数据长度
            chunk_header.chunk_id = chunk_id;
            chunk_header.total_chunks = total_chunks;
            chunk_header.file_size = file_size;
            strcpy(chunk_header.filename, filename.c_str());
            
            // 发送文件块
            if (!send_message(socket, chunk_header, buffer.data())) {
                cerr << "\n发送文件块 " << chunk_id << " 失败" << endl;
                file.close();
                return;
            }
            
            total_sent += bytes_read;
            chunk_id++;
            
            // 显示传输进度
            float progress = (float)chunk_id / total_chunks * 100;
            cout << "\r发送进度: " << fixed  << progress << "% "
                 << "(" << chunk_id << "/" << total_chunks << " 块)" << flush;
            
            // 适当的延迟，避免发送过快
            this_thread::sleep_for(chrono::microseconds(100));
        }
    }
    
    // 发送文件结束消息
    MessageHeader end_header = {0};
    end_header.type = FILE_END;
    end_header.data_length = 0;
    strcpy(end_header.filename, filename.c_str());
    send_message(socket, end_header);
    
    // 计算并显示传输统计信息
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;
    double speed_mbps = ((total_sent - start_chunk * chunk_size) / (1024.0 * 1024.0)) / seconds;
    
    cout << "\n文件发送完成: " << filename << endl;
    cout << "传输时间: " << seconds << " 秒" << endl;
    cout << "传输速度: " << fixed  << speed_mbps << " MB/s" << endl;
    
    file.close();
}

/**
 * 接收消息的线程函数
 * 在独立线程中运行，持续监听服务器发送的消息
 * @param socket 套接字描述符
 */
void receive_thread(int socket) {
    MessageHeader header;
    vector<char> data;
    
    cout << "[接收线程已启动]" << endl;
    
    // 无限循环，持续接收消息
    while (true) {
        // 接收消息
        if (!receive_message(socket, header, data)) {
            cout << "\n[接收线程] 连接已断开" << endl;
            break;
        }
        
        // 根据消息类型处理
        switch (header.type) {
            case TEXT_MSG: {
                // 文本消息
                string message(data.begin(), data.end());
                cout << "\n收到服务器消息: " << message << endl;
                cout << "send: " << flush;  // 恢复提示符
                break;
            }
            case FILE_START: {
                // 文件开始传输
                cout << "\n开始接收文件: " << header.filename 
                     << " (大小: " << header.file_size / (1024.0 * 1024.0) << " MB)" << endl;
                break;
            }
            case FILE_CHUNK: {
                // 文件块（这里可以扩展文件接收功能）
                float progress = (float)(header.chunk_id + 1) / header.total_chunks * 100;
                cout << "\r接收进度: " << fixed  << progress << "%" << flush;
                break;
            }
            case FILE_END: {
                // 文件结束
                cout << "\n文件接收完成: " << header.filename << endl;
                break;
            }
            default:
                cout << "\n收到未知类型消息: " << header.type << endl;
        }
    }
}

/**
 * 改进的IP地址验证函数
 * 验证标准IPv4地址格式（点分十进制）
 * @param ip_string IP地址字符串（格式：xxx.xxx.xxx.xxx）
 * @return IP地址格式正确返回true，否则返回false
 */
bool check_ip_valid(const string& ip_string) {
    // 使用字符串流分割IP地址
    stringstream ss(ip_string);
    string segment;
    vector<int> segments;
    
    // 按点分割
    while (getline(ss, segment, '.')) {
        try {
            int value = stoi(segment);
            // 检查每段是否在0-255范围内
            if (value < 0 || value > 255) {
                return false;
            }
            segments.push_back(value);
        } catch (const exception& e) {
            return false;  // 转换失败
        }
    }
    
    // IPv4地址应该有且仅有4段
    return segments.size() == 4;
}

/**
 * 主函数 - 程序入口点
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * 使用方式：
 *   1. 无参数运行：程序会提示输入IP和端口
 *   2. ./tcp_client <server_ip> [port]
 */
int main(int argc, char* argv[]) {
    /* ========== 输入处理部分 ========== */
    string server_ip;
    int port = 8888;  // 统一默认端口为8888
    
    // 情况1：无命令行参数，交互式输入
    if (argc == 1) {
        // 循环直到输入有效的IP地址
        while (true) {
            cout << "请输入服务器IP地址: ";
            cin >> server_ip;
            
            if (check_ip_valid(server_ip)) {
                break;  // IP格式正确
            } else {
                cerr << "IP地址格式无效，请输入正确的IPv4地址（如：192.168.1.1）" << endl;
            }
        }
        
        cout << "请输入服务器端口(默认8888): ";
        string port_input;
        cin.ignore();  // 忽略之前的换行符
        getline(cin, port_input);
        
        if (port_input.empty()) {
            port = 8888;  // 使用默认端口
        } else {
            try {
                port = stoi(port_input);
                if (port < 1 || port > 65535) {
                    cerr << "端口号无效，使用默认端口8888" << endl;
                    port = 8888;
                }
            } catch (const exception& e) {
                cerr << "端口号格式错误，使用默认端口8888" << endl;
                port = 8888;
            }
        }
    }
    
    // 情况2：支持命令行参数
    if (argc > 1) {
        server_ip = argv[1];
        if (!check_ip_valid(server_ip)) {
            cerr << "命令行提供的IP地址无效: " << server_ip << endl;
            return 1;
        }
    }
    if (argc > 2) {
        try {
            port = stoi(argv[2]);
            if (port < 1 || port > 65535) {
                cerr << "端口号必须在1-65535范围内" << endl;
                return 1;
            }
        } catch (const exception& e) {
            cerr << "端口号格式错误" << endl;
            return 1;
        }
    }
    
    /* ========== 建立连接部分 ========== */
    // 创建TCP socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        cerr << "创建socket失败: " << strerror(errno) << endl;
        return 1;
    }
    
    // 设置socket选项：启用地址重用
    int opt = 1;
    if (setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        cerr << "设置socket选项失败" << endl;
    }
    
    // 配置服务器地址结构体
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // 将IP地址字符串转换为网络字节序
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        cerr << "无效的服务器地址: " << server_ip << endl;
        close(client_socket);
        return 1;
    }
    
    // 连接到服务器
    cout << "正在连接服务器 " << server_ip << ":" << port << "..." << endl;
    if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "连接失败: " << strerror(errno) << endl;
        close(client_socket);
        return 1;
    }
    
    cout << "连接成功！" << endl;
    cout << "========================================" << endl;
    
    // 启动接收线程（只创建一次）
    thread recv_thread(receive_thread, client_socket);
    recv_thread.detach();
    
    /* ========== 主循环：处理用户命令 ========== */
    int command = 0;
    while (true) {
        // 显示菜单
        cout << "\n请选择操作:" << endl;
        cout << "========================================" << endl;
        cout << "1. 文本消息模式 (输入'exit'退出该模式)" << endl;
        cout << "2. 文件传输模式" << endl;
        cout << "3. 退出程序" << endl;
        cout << "========================================" << endl;
        cout << "请输入选项(1-3): ";
        
        cin >> command;
        cin.ignore();  // 清除输入缓冲区的换行符
        
        // 处理用户选择
        switch (command) {
            case 1: {
                // 文本消息模式
                cout << "\n进入文本消息模式（输入'exit'返回主菜单）" << endl;
                string send_str;
                
                while (true) {
                    cout << "send: ";
                    getline(cin, send_str);
                    
                    if (send_str == "exit") {
                        cout << "退出文本消息模式" << endl;
                        break;
                    }
                    
                    if (!send_str.empty()) {
                        send_text_message(client_socket, send_str);
                    }
                }
                break;
            }
            
            case 2: {
                // 文件传输模式
                cout << "\n进入文件传输模式" << endl;
                cout << "请输入要发送的文件路径: ";
                string file_path;
                getline(cin, file_path);
                
                if (!file_path.empty()) {
                    // 检查文件是否存在
                    struct stat file_stat;
                    if (stat(file_path.c_str(), &file_stat) == 0) {
                        // 检查是否是普通文件
                        if (S_ISREG(file_stat.st_mode)) {
                            // 在新线程中发送文件
                            thread file_thread(send_file_with_resume, client_socket, file_path);
                            file_thread.join();  // 等待文件传输完成
                        } else {
                            cerr << "错误：指定路径不是文件" << endl;
                        }
                    } else {
                        cerr << "错误：文件不存在 - " << file_path << endl;
                    }
                }
                break;
            }
            
            case 3: {
                // 退出程序
                cout << "\n正在断开连接..." << endl;
                
                // 发送断开连接消息（可选）
                MessageHeader disconnect_header = {0};
                disconnect_header.type = TEXT_MSG;
                string goodbye = "CLIENT_DISCONNECT";
                disconnect_header.data_length = goodbye.length();
                send_message(client_socket, disconnect_header, goodbye.c_str());
                
                // 给接收线程一些时间来处理最后的消息
                this_thread::sleep_for(chrono::milliseconds(500));
                
                cout << "关闭连接..." << endl;
                close(client_socket);
                cout << "程序已退出" << endl;
                return 0;
            }
            
            default:
                cerr << "无效的选项，请输入1-3" << endl;
                break;
        }
    }
    
    // 关闭socket连接
    close(client_socket);
    return 0;
}