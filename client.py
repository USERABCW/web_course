#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tcp_client.py
Python TCP客户端 - 与C++版本完全兼容
支持功能：
1. 文本消息收发
2. 大文件传输
3. 断点续传
4. 实时进度显示
5. 传输速度统计
6. 命令行交互界面

消息格式：[总长度(4字节)][消息头][数据部分]
与C++客户端和服务器保持完全一致的协议格式
"""

import socket
import struct
import threading
import time
import os
import sys
from pathlib import Path
import logging
from typing import Optional, Tuple
import select
import argparse
import ipaddress

# ========== 日志配置 ==========
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - [%(levelname)s] - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# ========== 消息类型常量 ==========
# 必须与C++版本保持一致
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


class TCPClient:
    """
    TCP客户端类
    实现与C++版本相同的功能
    """
    
    def __init__(self, server_host: str = '127.0.0.1', server_port: int = 8888):
        """
        初始化客户端
        
        Args:
            server_host: 服务器地址
            server_port: 服务器端口
        """
        self.server_host = server_host
        self.server_port = server_port
        self.socket = None
        self.connected = False
        self.receive_thread = None
        self.running = True
        
    def validate_ip(self, ip_string: str) -> bool:
        """
        验证IP地址格式
        
        Args:
            ip_string: IP地址字符串
            
        Returns:
            IP地址格式正确返回True，否则返回False
        """
        try:
            ipaddress.ip_address(ip_string)
            return True
        except ValueError:
            return False
    
    def connect(self) -> bool:
        """
        连接到服务器
        
        Returns:
            连接成功返回True，失败返回False
        """
        try:
            # 验证IP地址
            if not self.validate_ip(self.server_host):
                logger.error(f"无效的IP地址: {self.server_host}")
                return False
            
            # 创建TCP socket
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            
            # 设置socket选项
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            logger.info(f"正在连接服务器 {self.server_host}:{self.server_port}...")
            
            # 连接到服务器
            self.socket.connect((self.server_host, self.server_port))
            self.connected = True
            
            logger.info("连接成功！")
            logger.info("=" * 50)
            
            # 启动接收线程
            self.receive_thread = threading.Thread(
                target=self.receive_messages,
                daemon=True  # 守护线程
            )
            self.receive_thread.start()
            logger.info("[接收线程已启动]")
            
            return True
            
        except ConnectionRefusedError:
            logger.error(f"连接被拒绝：服务器可能未启动或地址错误")
            return False
        except socket.timeout:
            logger.error(f"连接超时：服务器无响应")
            return False
        except Exception as e:
            logger.error(f"连接失败: {e}")
            return False
    
    def disconnect(self):
        """
        断开与服务器的连接
        """
        self.running = False
        self.connected = False
        
        # 发送断开连接消息
        if self.socket:
            try:
                disconnect_msg = "CLIENT_DISCONNECT"
                self.send_text(disconnect_msg)
                time.sleep(0.5)  # 给一些时间发送消息
            except:
                pass
            
            try:
                self.socket.close()
            except:
                pass
                
        logger.info("已断开连接")
        logger.info("=" * 50)
    
    def send_message(self, msg_type: int, filename: str = '',
                    data_length: int = 0, chunk_id: int = 0,
                    total_chunks: int = 0, file_size: int = 0,
                    data: bytes = None) -> bool:
        """
        发送消息（与C++版本格式一致）
        消息格式：[总长度(4字节)][消息头][数据]
        
        Args:
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
            
            # 计算总长度（消息头 + 数据）
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
            
            # 一次性发送整个消息
            self.socket.sendall(message)
            return True
            
        except BrokenPipeError:
            logger.error("连接已断开(EPIPE)")
            self.connected = False
            return False
        except Exception as e:
            logger.error(f"发送消息失败: {e}")
            return False
    
    def receive_all(self, length: int) -> Optional[bytes]:
        """
        接收指定长度的数据
        
        Args:
            length: 要接收的字节数
            
        Returns:
            接收到的数据，失败返回None
        """
        data = b''
        while len(data) < length:
            try:
                chunk = self.socket.recv(min(4096, length - len(data)))
                if not chunk:
                    return None  # 连接断开
                data += chunk
            except socket.timeout:
                continue
            except Exception:
                return None
        return data
    
    def receive_message(self) -> Tuple[Optional[int], Optional[dict], Optional[bytes]]:
        """
        接收消息（与服务器格式一致）
        
        Returns:
            (消息类型, 消息头字典, 数据) 或 (None, None, None) 如果失败
        """
        try:
            # 1. 接收总长度（4字节）
            length_data = self.receive_all(4)
            if not length_data:
                return None, None, None
            
            # 解析总长度（网络字节序）
            total_length = struct.unpack('!I', length_data)[0]
            
            # 验证长度合理性
            if total_length < MESSAGE_HEADER_SIZE or total_length > 10 * 1024 * 1024:
                logger.error(f"接收到无效的消息长度: {total_length}")
                return None, None, None
            
            # 2. 接收完整消息（消息头 + 数据）
            message_data = self.receive_all(total_length)
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
    
    def receive_messages(self):
        """
        接收消息的线程函数
        持续监听服务器发送的消息
        """
        logger.debug("接收线程开始运行")
        
        while self.connected and self.running:
            try:
                # 设置socket超时，避免无限阻塞
                self.socket.settimeout(1.0)
                
                # 接收消息
                msg_type, header, data = self.receive_message()
                
                if msg_type is None:
                    if self.connected:  # 非主动断开
                        logger.info("\n[接收线程] 连接已断开")
                        self.connected = False
                    break
                
                # 根据消息类型处理
                if msg_type == TEXT_MSG:
                    # 文本消息
                    if data:
                        message = data.decode('utf-8', errors='ignore')
                        print(f"\n收到服务器消息: {message}")
                        print("send: ", end='', flush=True)
                        
                elif msg_type == FILE_START:
                    # 服务器开始发送文件
                    filename = header['filename']
                    file_size = header['file_size']
                    total_chunks = header['total_chunks']
                    print(f"\n开始接收文件: {filename}")
                    print(f"  文件大小: {file_size/(1024*1024):.2f} MB")
                    print(f"  总块数: {total_chunks}")
                    
                elif msg_type == FILE_CHUNK:
                    # 文件数据块
                    filename = header['filename']
                    chunk_id = header['chunk_id']
                    total_chunks = header['total_chunks']
                    progress = (chunk_id + 1) / total_chunks * 100
                    print(f"\r接收进度: {progress:.2f}% ({chunk_id + 1}/{total_chunks} 块)", 
                          end='', flush=True)
                    
                elif msg_type == FILE_END:
                    # 文件接收完成
                    filename = header['filename']
                    print(f"\n文件接收完成: {filename}")
                    
                else:
                    logger.warning(f"收到未知类型消息: {msg_type}")
                    
            except socket.timeout:
                continue  # 超时是正常的
            except Exception as e:
                if self.connected and self.running:
                    if "timed out" not in str(e):
                        logger.error(f"接收消息错误: {e}")
                break
        
        logger.debug("接收线程结束")
    
    def send_text(self, message: str):
        """
        发送文本消息
        
        Args:
            message: 要发送的文本消息
        """
        data = message.encode('utf-8')
        if self.send_message(TEXT_MSG, data_length=len(data), data=data):
            logger.info(f"文本消息已发送: {message}")
        else:
            logger.error("发送文本消息失败")
    
    def send_file(self, filepath: str):
        """
        发送文件（支持断点续传）
        将文件分块发送，支持大文件传输
        
        Args:
            filepath: 要发送的文件路径
        """
        try:
            # 检查文件是否存在
            if not os.path.exists(filepath):
                logger.error(f"文件不存在: {filepath}")
                return
            
            # 检查是否是文件
            if not os.path.isfile(filepath):
                logger.error(f"指定路径不是文件: {filepath}")
                return
            
            # 获取文件信息
            file_size = os.path.getsize(filepath)
            filename = os.path.basename(filepath)
            
            # 计算分块信息
            chunk_size = 4096  # 4KB每块（与C++版本一致）
            total_chunks = (file_size + chunk_size - 1) // chunk_size
            
            logger.info(f"准备发送文件: {filename}")
            logger.info(f"  文件大小: {file_size/(1024*1024):.2f} MB")
            logger.info(f"  总块数: {total_chunks}")
            
            # 发送文件开始消息
            if not self.send_message(FILE_START, filename=filename,
                                    file_size=file_size, total_chunks=total_chunks):
                logger.error("发送文件开始消息失败")
                return
            
            # 检查是否需要断点续传
            start_chunk = 0
            
            # 使用select等待服务器响应（最多等待2秒）
            readable, _, _ = select.select([self.socket], [], [], 2)
            
            if readable:
                try:
                    # 尝试接收服务器的续传请求
                    msg_type, header, _ = self.receive_message()
                    if msg_type == FILE_RESUME:
                        start_chunk = header['chunk_id']
                        logger.info(f"服务器请求断点续传，从块 {start_chunk} 开始")
                except:
                    pass  # 没有续传请求，从头开始
            
            # 发送文件块
            start_time = time.time()
            total_sent = start_chunk * chunk_size
            
            with open(filepath, 'rb') as f:
                # 跳转到续传位置
                if start_chunk > 0:
                    f.seek(start_chunk * chunk_size)
                
                chunk_id = start_chunk
                while chunk_id < total_chunks:
                    # 读取一块数据
                    chunk_data = f.read(chunk_size)
                    if not chunk_data:
                        break
                    
                    # 发送文件块
                    if not self.send_message(FILE_CHUNK, filename=filename,
                                           chunk_id=chunk_id,
                                           total_chunks=total_chunks,
                                           data=chunk_data):
                        logger.error(f"\n发送文件块 {chunk_id} 失败")
                        return
                    
                    total_sent += len(chunk_data)
                    chunk_id += 1
                    
                    # 显示进度（每10个块或最后一块时更新）
                    if chunk_id % 10 == 0 or chunk_id == total_chunks:
                        progress = chunk_id / total_chunks * 100
                        print(f"\r发送进度: {progress:.2f}% ({chunk_id}/{total_chunks} 块)",
                              end='', flush=True)
                    
                    # 适当延迟，避免发送过快
                    time.sleep(0.0001)  # 100微秒
            
            print()  # 换行
            
            # 发送文件结束消息
            self.send_message(FILE_END, filename=filename)
            
            # 计算传输统计
            duration = time.time() - start_time
            actual_sent = total_sent - (start_chunk * chunk_size)
            speed_mbps = (actual_sent / (1024 * 1024)) / duration if duration > 0 else 0
            
            logger.info(f"文件发送完成: {filename}")
            logger.info(f"  传输时间: {duration:.2f} 秒")
            logger.info(f"  传输速度: {speed_mbps:.2f} MB/s")
            
        except Exception as e:
            logger.error(f"发送文件失败: {e}")


def create_test_file(filename: str = "test_file.bin", size_mb: int = 10) -> str:
    """
    创建测试文件
    
    Args:
        filename: 文件名
        size_mb: 文件大小（MB）
        
    Returns:
        创建的文件路径
    """
    size_bytes = size_mb * 1024 * 1024
    logger.info(f"正在创建测试文件 {filename} ({size_mb}MB)...")
    
    with open(filename, 'wb') as f:
        # 写入随机数据，每次1MB
        chunk_size = 1024 * 1024
        for i in range(size_mb):
            # 使用随机数据
            data = os.urandom(chunk_size)
            f.write(data)
            # 显示进度
            progress = (i + 1) / size_mb * 100
            print(f"\r创建进度: {progress:.1f}%", end='', flush=True)
    
    print()  # 换行
    logger.info(f"测试文件创建完成: {filename}")
    return filename


def interactive_mode(client: TCPClient):
    """
    交互式命令行模式
    
    Args:
        client: TCP客户端对象
    """
    print("\n" + "=" * 50)
    print("进入交互模式")
    print("=" * 50)
    
    while True:
        print("\n请选择操作:")
        print("1. 文本消息模式 (输入'exit'退出该模式)")
        print("2. 文件传输模式")
        print("3. 创建测试文件")
        print("4. 退出程序")
        print("-" * 30)
        
        try:
            choice = input("请输入选项(1-4): ").strip()
            
            if choice == '1':
                # 文本消息模式
                print("\n进入文本消息模式（输入'exit'返回主菜单）")
                while True:
                    message = input("send: ").strip()
                    if message.lower() == 'exit':
                        print("退出文本消息模式")
                        break
                    if message:
                        client.send_text(message)
                        
            elif choice == '2':
                # 文件传输模式
                print("\n进入文件传输模式")
                filepath = input("请输入要发送的文件路径: ").strip()
                if filepath:
                    # 在新线程中发送文件，避免阻塞主线程
                    thread = threading.Thread(
                        target=client.send_file,
                        args=(filepath,)
                    )
                    thread.start()
                    thread.join()  # 等待文件传输完成
                    
            elif choice == '3':
                # 创建测试文件
                size_input = input("请输入测试文件大小(MB, 默认10): ").strip()
                try:
                    size_mb = int(size_input) if size_input else 10
                    if 1 <= size_mb <= 1000:
                        filename = create_test_file(size_mb=size_mb)
                        print(f"提示：可以使用文件传输模式发送 {filename}")
                    else:
                        print("文件大小应在 1-1000 MB 之间")
                except ValueError:
                    print("无效的大小输入")
                    
            elif choice == '4':
                print("\n正在退出...")
                break
                
            else:
                print("无效的选项，请输入1-4")
                
        except KeyboardInterrupt:
            print("\n\n收到中断信号")
            break
        except Exception as e:
            logger.error(f"操作错误: {e}")


def main():
    """
    主函数
    支持命令行参数和交互式输入
    """
    # 创建参数解析器
    parser = argparse.ArgumentParser(
        description='Python TCP客户端 - 与C++版本兼容',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python tcp_client.py                      # 交互式输入服务器信息
  python tcp_client.py 192.168.1.100        # 连接到指定IP（默认端口8888）
  python tcp_client.py 192.168.1.100 9999   # 连接到指定IP和端口
  python tcp_client.py --host 127.0.0.1 --port 8888  # 使用命名参数
        """
    )
    
    # 位置参数（与C++版本一致）
    parser.add_argument('server_ip', nargs='?', help='服务器IP地址')
    parser.add_argument('server_port', type=int, nargs='?', help='服务器端口')
    
    # 命名参数
    parser.add_argument('--host', help='服务器地址（优先级低于位置参数）')
    parser.add_argument('--port', type=int, help='服务器端口（优先级低于位置参数）')
    
    # 解析参数
    args = parser.parse_args()
    
    # 确定服务器地址和端口
    server_host = None
    server_port = None
    
    # 优先使用位置参数
    if args.server_ip:
        server_host = args.server_ip
    elif args.host:
        server_host = args.host
        
    if args.server_port:
        server_port = args.server_port
    elif args.port:
        server_port = args.port
    
    # 如果没有提供参数，交互式输入
    if not server_host:
        while True:
            server_host = input("请输入服务器IP地址: ").strip()
            # 验证IP地址格式
            try:
                ipaddress.ip_address(server_host)
                break
            except ValueError:
                print(f"IP地址格式无效，请输入正确的IPv4或IPv6地址")
    
    if not server_port:
        port_input = input("请输入服务器端口(默认8888): ").strip()
        if port_input:
            try:
                server_port = int(port_input)
                if server_port < 1 or server_port > 65535:
                    print("端口号必须在1-65535范围内，使用默认端口8888")
                    server_port = 8888
            except ValueError:
                print("端口号格式错误，使用默认端口8888")
                server_port = 8888
        else:
            server_port = 8888
    
    # 创建客户端
    client = TCPClient(server_host, server_port)
    
    # 连接服务器
    if not client.connect():
        sys.exit(1)
    
    try:
        # 进入交互模式
        interactive_mode(client)
        
    except KeyboardInterrupt:
        print("\n\n收到中断信号，正在退出...")
        
    finally:
        # 断开连接
        client.disconnect()
        print("客户端已退出")


if __name__ == '__main__':
    main()