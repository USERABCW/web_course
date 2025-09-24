#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tcp_server.py
Python TCP服务器 - 与C++版本完全兼容
支持功能：
1. 多客户端并发连接
2. 文本消息收发
3. 大文件传输
4. 断点续传
5. 实时进度显示
6. 传输速度统计

消息格式：[总长度(4字节)][消息头][数据部分]
与C++版本保持完全一致的协议格式
"""

import socket
import struct
import threading
import time
import os
import signal
import sys
from pathlib import Path
from typing import Dict, Tuple, Optional, Any
import logging
from datetime import datetime

# ========== 日志配置 ==========
# 设置日志格式，包含时间戳和消息内容
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - [%(levelname)s] - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# ========== 全局变量 ==========
server_running = True              # 服务器运行标志
clients_lock = threading.Lock()   # 保护客户端列表的互斥锁
active_clients = {}               # 活跃客户端字典 {地址: socket}
file_transfers_lock = threading.Lock()  # 保护文件传输状态的互斥锁

# ========== 消息类型常量 ==========
# 必须与C++客户端保持一致
TEXT_MSG = 1        # 文本消息
FILE_START = 2      # 文件传输开始
FILE_CHUNK = 3      # 文件数据块
FILE_END = 4        # 文件传输结束
FILE_RESUME = 5     # 断点续传请求

# ========== 消息头结构定义 ==========
# 与C++的MessageHeader结构体完全对应
# struct MessageHeader {
#     uint32_t type;           // 消息类型
#     uint32_t data_length;    // 数据长度
#     uint32_t chunk_id;       // 块编号
#     uint32_t total_chunks;   // 总块数
#     uint64_t file_size;      // 文件大小
#     char filename[256];      // 文件名
# };
MESSAGE_HEADER_FORMAT = '!IIIIQ256s'  # !表示网络字节序（大端）
MESSAGE_HEADER_SIZE = struct.calcsize(MESSAGE_HEADER_FORMAT)


class FileTransferState:
    """
    文件传输状态类
    用于跟踪每个文件的传输进度和状态
    """
    
    def __init__(self, filename: str, total_size: int, total_chunks: int):
        """
        初始化文件传输状态
        
        Args:
            filename: 文件名
            total_size: 文件总大小（字节）
            total_chunks: 总块数
        """
        self.filename = filename
        self.total_size = total_size
        self.total_chunks = total_chunks
        self.received_chunks = set()      # 已接收的块集合
        self.received_size = 0             # 已接收的字节数
        self.start_time = time.time()      # 开始时间
        self.file_path = f"received_{filename}"  # 保存路径
        self.file = None                   # 文件对象
        self.lock = threading.Lock()       # 状态锁


class TCPServer:
    """
    TCP服务器类
    实现与C++版本相同的功能
    """
    
    def __init__(self, host: str = '0.0.0.0', port: int = 8888):
        """
        初始化服务器
        
        Args:
            host: 监听地址，默认监听所有接口
            port: 监听端口，默认8888
        """
        self.host = host
        self.port = port
        self.socket = None
        # 文件传输状态字典 {(客户端IP, 文件名): FileTransferState}
        self.file_transfers: Dict[Tuple[str, str], FileTransferState] = {}
        
    def signal_handler(self, sig, frame):
        """
        信号处理函数
        优雅地关闭服务器（响应Ctrl+C）
        
        Args:
            sig: 信号编号
            frame: 当前栈帧
        """
        global server_running
        logger.info("\n收到中断信号，服务器正在关闭...")
        server_running = False
        
        # 关闭所有客户端连接
        with clients_lock:
            for addr, client_socket in active_clients.items():
                try:
                    client_socket.close()
                except:
                    pass
            active_clients.clear()
        
        # 关闭服务器socket
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        
        logger.info("服务器已安全关闭")
        sys.exit(0)
    
    def start(self):
        """
        启动服务器
        主循环，接受客户端连接
        """
        # 设置信号处理（Ctrl+C）
        signal.signal(signal.SIGINT, self.signal_handler)
        
        try:
            # 创建TCP socket
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            
            # 设置socket选项：允许地址重用
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # 绑定地址和端口
            self.socket.bind((self.host, self.port))
            
            # 开始监听，队列长度为10
            self.socket.listen(10)
            
            logger.info("=" * 50)
            logger.info("TCP文件传输服务器（Python版）")
            logger.info(f"监听地址: {self.host}:{self.port}")
            logger.info("按 Ctrl+C 优雅关闭服务器")
            logger.info("=" * 50)
            
            # 主循环：接受客户端连接
            while server_running:
                try:
                    # 设置超时，以便定期检查server_running标志
                    self.socket.settimeout(1.0)
                    
                    try:
                        # 接受新连接
                        client_socket, client_addr = self.socket.accept()
                        
                        # 为每个客户端创建独立线程
                        client_thread = threading.Thread(
                            target=self.handle_client,
                            args=(client_socket, client_addr),
                            daemon=True  # 守护线程，主程序退出时自动结束
                        )
                        client_thread.start()
                        
                        # 记录活跃客户端
                        with clients_lock:
                            active_clients[client_addr] = client_socket
                            
                    except socket.timeout:
                        continue  # 超时是正常的，继续循环
                        
                except Exception as e:
                    if server_running:
                        logger.error(f"接受连接时出错: {e}")
                        
        except Exception as e:
            logger.error(f"服务器错误: {e}")
            
        finally:
            # 清理资源
            if self.socket:
                self.socket.close()
            logger.info("服务器已关闭")
    
    def handle_client(self, client_socket: socket.socket, client_addr: Tuple[str, int]):
        """
        处理单个客户端连接
        在独立线程中运行
        
        Args:
            client_socket: 客户端socket对象
            client_addr: 客户端地址(IP, 端口)
        """
        client_ip = client_addr[0]
        client_port = client_addr[1]
        
        logger.info(f"\n[新连接] 客户端: {client_ip}:{client_port}")
        logger.info("=" * 50)
        
        try:
            # 设置socket超时，避免无限阻塞
            client_socket.settimeout(30.0)
            
            while server_running:
                try:
                    # 接收消息
                    msg_type, msg_header, msg_data = self.receive_message(client_socket)
                    
                    if msg_type is None:
                        break  # 连接断开
                    
                    # 根据消息类型处理
                    if msg_type == TEXT_MSG:
                        self.handle_text_message(client_socket, client_addr, msg_data, msg_header)
                    elif msg_type == FILE_START:
                        self.handle_file_start(client_socket, client_addr, msg_header)
                    elif msg_type == FILE_CHUNK:
                        self.handle_file_chunk(client_addr, msg_header, msg_data)
                    elif msg_type == FILE_END:
                        self.handle_file_end(client_addr, msg_header)
                    else:
                        logger.warning(f"[{client_ip}] 收到未知消息类型: {msg_type}")
                        
                except socket.timeout:
                    # 超时，检查是否需要继续
                    continue
                    
                except Exception as e:
                    if "timed out" not in str(e):
                        logger.error(f"[{client_ip}] 处理消息时出错: {e}")
                    break
                    
        except Exception as e:
            logger.error(f"[{client_ip}] 客户端处理异常: {e}")
            
        finally:
            # 清理资源
            # 关闭未完成的文件传输
            keys_to_remove = []
            with file_transfers_lock:
                for key in self.file_transfers:
                    if key[0] == client_ip:
                        state = self.file_transfers[key]
                        if state.file:
                            state.file.close()
                        keys_to_remove.append(key)
                        
                for key in keys_to_remove:
                    del self.file_transfers[key]
            
            # 从活跃客户端列表中移除
            with clients_lock:
                if client_addr in active_clients:
                    del active_clients[client_addr]
            
            # 关闭socket
            client_socket.close()
            logger.info(f"[断开连接] 客户端: {client_ip}:{client_port}")
    
    def receive_message(self, sock: socket.socket) -> Tuple[Optional[int], Optional[dict], Optional[bytes]]:
        """
        接收消息（与C++版本格式一致）
        消息格式：[总长度(4字节)][消息头][数据]
        
        Args:
            sock: socket对象
            
        Returns:
            (消息类型, 消息头字典, 数据) 或 (None, None, None) 如果失败
        """
        try:
            # 1. 接收总长度（4字节）
            length_data = self.receive_all(sock, 4)
            if not length_data:
                return None, None, None
            
            # 解析总长度（网络字节序）
            total_length = struct.unpack('!I', length_data)[0]
            
            # 验证长度合理性
            if total_length < MESSAGE_HEADER_SIZE or total_length > 10 * 1024 * 1024:
                logger.error(f"接收到无效的消息长度: {total_length}")
                return None, None, None
            
            # 2. 接收完整消息（消息头 + 数据）
            message_data = self.receive_all(sock, total_length)
            if not message_data:
                return None, None, None
            
            # 3. 解析消息头
            header_data = message_data[:MESSAGE_HEADER_SIZE]
            msg_type, data_length, chunk_id, total_chunks, file_size, filename_bytes = \
                struct.unpack(MESSAGE_HEADER_FORMAT, header_data)
            
            # 处理文件名
            filename = filename_bytes.decode('utf-8', errors='ignore').rstrip('\x00')
            
            # 4. 提取数据部分
            data = None
            if data_length > 0:
                data = message_data[MESSAGE_HEADER_SIZE:]
            
            # 构造消息头字典
            header = {
                'type': msg_type,
                'data_length': data_length,
                'chunk_id': chunk_id,
                'total_chunks': total_chunks,
                'file_size': file_size,
                'filename': filename
            }
            
            return msg_type, header, data
            
        except Exception as e:
            logger.error(f"接收消息失败: {e}")
            return None, None, None
    
    def send_message(self, sock: socket.socket, msg_type: int,
                    filename: str = '', data_length: int = 0,
                    chunk_id: int = 0, total_chunks: int = 0,
                    file_size: int = 0, data: bytes = None) -> bool:
        """
        发送消息（与C++版本格式一致）
        消息格式：[总长度(4字节)][消息头][数据]
        
        Args:
            sock: socket对象
            msg_type: 消息类型
            filename: 文件名
            data_length: 数据长度
            chunk_id: 块编号
            total_chunks: 总块数
            file_size: 文件大小
            data: 数据内容
            
        Returns:
            成功返回True，失败返回False
        """
        try:
            # 计算实际数据长度
            if data:
                data_length = len(data)
            
            # 计算总长度
            total_length = MESSAGE_HEADER_SIZE + data_length
            
            # 构造消息
            # 1. 总长度（4字节，网络字节序）
            length_bytes = struct.pack('!I', total_length)
            
            # 2. 消息头
            filename_bytes = filename.encode('utf-8')[:255].ljust(256, b'\x00')
            header = struct.pack(MESSAGE_HEADER_FORMAT,
                               msg_type, data_length, chunk_id, total_chunks,
                               file_size, filename_bytes)
            
            # 3. 组合消息
            message = length_bytes + header
            if data:
                message += data
            
            # 发送完整消息
            sock.sendall(message)
            return True
            
        except Exception as e:
            logger.error(f"发送消息失败: {e}")
            return False
    
    def receive_all(self, sock: socket.socket, length: int) -> Optional[bytes]:
        """
        接收指定长度的数据
        
        Args:
            sock: socket对象
            length: 要接收的字节数
            
        Returns:
            接收到的数据，失败返回None
        """
        data = b''
        while len(data) < length:
            try:
                chunk = sock.recv(min(4096, length - len(data)))
                if not chunk:
                    return None  # 连接断开
                data += chunk
            except Exception:
                return None
        return data
    
    def handle_text_message(self, client_socket: socket.socket,
                          client_addr: Tuple[str, int],
                          data: bytes, header: dict):
        """
        处理文本消息
        
        Args:
            client_socket: 客户端socket
            client_addr: 客户端地址
            data: 消息数据
            header: 消息头
        """
        client_ip = client_addr[0]
        
        # 解码消息
        message = data.decode('utf-8', errors='ignore')
        
        # 检查是否是断开连接消息
        if message == "CLIENT_DISCONNECT":
            logger.info(f"[{client_ip}] 客户端主动断开连接")
            return
        
        logger.info(f"[{client_ip}] 文本消息: {message}")
        
        # 发送回复
        reply = f"服务器已收到: {message}"
        reply_bytes = reply.encode('utf-8')
        
        if not self.send_message(client_socket, TEXT_MSG,
                                data_length=len(reply_bytes),
                                data=reply_bytes):
            logger.error(f"[{client_ip}] 发送回复失败")
    
    def handle_file_start(self, client_socket: socket.socket,
                         client_addr: Tuple[str, int], header: dict):
        """
        处理文件传输开始
        
        Args:
            client_socket: 客户端socket
            client_addr: 客户端地址
            header: 消息头
        """
        client_ip = client_addr[0]
        filename = header['filename']
        file_size = header['file_size']
        total_chunks = header['total_chunks']
        
        logger.info(f"\n[{client_ip}] 开始接收文件: {filename}")
        logger.info(f"  文件大小: {file_size/(1024*1024):.2f} MB")
        logger.info(f"  总块数: {total_chunks}")
        
        # 创建文件传输状态
        key = (client_ip, filename)
        state = FileTransferState(filename, file_size, total_chunks)
        
        # 检查断点续传
        if os.path.exists(state.file_path):
            existing_size = os.path.getsize(state.file_path)
            
            if existing_size < file_size:
                # 支持断点续传
                state.received_size = existing_size
                state.file = open(state.file_path, 'ab')
                
                # 计算已接收的块数（假设每块4KB）
                chunk_size = 4096
                resume_chunk = existing_size // chunk_size
                
                # 发送续传请求
                logger.info(f"  [断点续传] 从块 {resume_chunk} 继续接收")
                self.send_message(client_socket, FILE_RESUME,
                                filename=filename,
                                chunk_id=resume_chunk)
                
                # 更新已接收的块
                for i in range(resume_chunk):
                    state.received_chunks.add(i)
            else:
                # 文件已存在且大小相同或更大，重新接收
                state.file = open(state.file_path, 'wb')
        else:
            # 新文件
            state.file = open(state.file_path, 'wb')
        
        # 保存状态
        with file_transfers_lock:
            self.file_transfers[key] = state
    
    def handle_file_chunk(self, client_addr: Tuple[str, int],
                         header: dict, data: bytes):
        """
        处理文件数据块
        
        Args:
            client_addr: 客户端地址
            header: 消息头
            data: 数据块
        """
        client_ip = client_addr[0]
        filename = header['filename']
        chunk_id = header['chunk_id']
        total_chunks = header['total_chunks']
        
        key = (client_ip, filename)
        
        with file_transfers_lock:
            if key not in self.file_transfers:
                logger.error(f"[{client_ip}] 错误：收到未知文件的数据块")
                return
            
            state = self.file_transfers[key]
        
        # 写入文件
        with state.lock:
            if state.file:
                state.file.write(data)
                state.file.flush()  # 确保数据写入磁盘
                
                state.received_chunks.add(chunk_id)
                state.received_size += len(data)
        
        # 显示进度（每10个块或最后一块时更新）
        if chunk_id % 10 == 0 or chunk_id == total_chunks - 1:
            progress = (chunk_id + 1) / total_chunks * 100
            print(f"\r[{client_ip}] {filename} 进度: {progress:.2f}% "
                  f"({chunk_id + 1}/{total_chunks} 块)", end='', flush=True)
            
            # 最后一块时换行
            if chunk_id == total_chunks - 1:
                print()
    
    def handle_file_end(self, client_addr: Tuple[str, int], header: dict):
        """
        处理文件传输结束
        
        Args:
            client_addr: 客户端地址
            header: 消息头
        """
        client_ip = client_addr[0]
        filename = header['filename']
        
        key = (client_ip, filename)
        
        with file_transfers_lock:
            if key not in self.file_transfers:
                return
            
            state = self.file_transfers[key]
            
            # 关闭文件
            if state.file:
                state.file.close()
            
            # 计算传输统计
            duration = time.time() - state.start_time
            speed_mbps = (state.received_size / (1024 * 1024)) / duration if duration > 0 else 0
            
            logger.info(f"\n[{client_ip}] 文件接收完成: {filename}")
            logger.info(f"  保存为: {state.file_path}")
            logger.info(f"  文件大小: {state.received_size/(1024*1024):.2f} MB")
            logger.info(f"  传输时间: {duration:.2f} 秒")
            logger.info(f"  传输速度: {speed_mbps:.2f} MB/s")
            logger.info("=" * 50)
            
            # 删除传输状态
            del self.file_transfers[key]


def main():
    """
    主函数
    支持命令行参数
    """
    import argparse
    
    # 创建参数解析器
    parser = argparse.ArgumentParser(
        description='Python TCP服务器 - 与C++版本兼容',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python tcp_server.py                    # 使用默认设置(0.0.0.0:8888)
  python tcp_server.py --port 9999        # 指定端口
  python tcp_server.py --host 127.0.0.1   # 只监听本地连接
        """
    )
    
    parser.add_argument('--host', 
                       default='0.0.0.0',
                       help='监听地址（默认: 0.0.0.0，监听所有接口）')
    
    parser.add_argument('--port', 
                       type=int,
                       default=8888,
                       help='监听端口（默认: 8888）')
    
    # 解析参数
    args = parser.parse_args()
    
    # 验证端口范围
    if args.port < 1 or args.port > 65535:
        logger.error(f"端口号必须在1-65535范围内")
        sys.exit(1)
    
    # 创建并启动服务器
    try:
        server = TCPServer(args.host, args.port)
        server.start()
    except KeyboardInterrupt:
        logger.info("\n服务器已停止")
    except Exception as e:
        logger.error(f"服务器启动失败: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()