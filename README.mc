# TCP文件传输系统

## 项目说明

这是一个支持C++和Python互操作的TCP文件传输系统，具有以下特性：

- **跨语言通信**：C++和Python实现的客户端和服务器可以相互通信
- **全双工通信**：支持双向文本消息和文件传输
- **断点续传**：文件传输中断后可以继续传输，无需重新开始
- **多线程处理**：每个客户端连接都在独立线程中处理
- **传输监控**：实时显示传输进度和速度
- **错误处理**：完善的错误处理机制，不会导致程序崩溃

## 编译和运行

### C++版本编译

使用提供的Makefile：

```bash
make all        # 编译所有C++程序
make server     # 只编译服务器
make client     # 只编译客户端
make clean      # 清理编译文件
```

或者手动编译：

```bash
# 编译服务器
g++ -std=c++11 -pthread tcp_server.cpp -o tcp_server

# 编译客户端
g++ -std=c++11 -pthread tcp_client.cpp -o tcp_client
```

### 运行服务器

C++版本：
```bash
./tcp_server [端口号]
# 例如：
./tcp_server 8888
```

Python版本：
```bash
python3 tcp_server.py --host 0.0.0.0 --port 8888
# 或使用默认设置：
python3 tcp_server.py
```

### 运行客户端

C++版本：
```bash
./tcp_client [服务器IP] [端口号]
# 例如：
./tcp_client 127.0.0.1 8888
```

Python版本：
```bash
python3 tcp_client.py --host 127.0.0.1 --port 8888
# 或使用默认设置：
python3 tcp_client.py
```

## 使用说明

### 客户端命令

连接成功后，可以使用以下命令：

- `text <消息>` - 发送文本消息到服务器
- `file <文件路径>` - 发送文件（支持断点续传）
- `help` - 显示帮助信息
- `quit` 或 `exit` - 退出客户端

Python客户端额外命令：
- `create <大小MB>` - 创建指定大小的测试文件

### 使用示例

1. **发送文本消息**：
```
请输入命令> text Hello, Server!
```

2. **发送文件**：
```
请输入命令> file /path/to/largefile.zip
```

3. **创建测试文件（Python客户端）**：
```
请输入命令> create 10
请输入命令> file test_file.bin
```

## 断点续传功能

系统自动支持断点续传：

1. 如果文件传输中断，服务器会保留已接收的部分
2. 重新传输同一文件时，系统会自动检测并从断点继续
3. 客户端会显示"从块 X 开始"的提示信息

## 性能测试

建议使用大于1MB的文件进行测试，以准确测量传输速度：

1. 使用Python客户端创建测试文件：
```bash
python3 tcp_client.py
> create 100  # 创建100MB测试文件
> file test_file.bin
```

2. 或使用系统命令创建测试文件：
```bash
# Linux/Mac
dd if=/dev/urandom of=test_file.bin bs=1M count=100

# Windows (PowerShell)
$file = New-Object byte[] (100MB)
(New-Object Random).NextBytes($file)
[IO.File]::WriteAllBytes("test_file.bin", $file)
```

## 跨语言兼容性测试

测试所有组合以确保兼容性：

1. **C++服务器 + C++客户端**
2. **C++服务器 + Python客户端**
3. **Python服务器 + C++客户端**
4. **Python服务器 + Python客户端**

所有组合都应该能够：
- 发送和接收文本消息
- 传输大文件（>1MB）
- 支持断点续传
- 显示传输进度和速度

## 文件结构

```
project/
├── tcp_server.cpp      # C++服务器实现
├── tcp_client.cpp      # C++客户端实现
├── tcp_server.py       # Python服务器实现
├── tcp_client.py       # Python客户端实现
├── Makefile           # C++编译配置
└── README.md          # 本文档
```

## 技术细节

### 消息协议

使用统一的消息头结构（272字节）：
- 消息类型 (4字节)
- 消息长度 (4字节)
- 块ID (4字节)
- 总块数 (4字节)
- 文件名 (256字节)

### 消息类型

- `TEXT_MSG (1)`: 文本消息
- `FILE_START (2)`: 文件传输开始
- `FILE_CHUNK (3)`: 文件数据块
- `FILE_END (4)`: 文件传输结束
- `FILE_RESUME (5)`: 断点续传请求

### 文件分块

- 默认块大小：1MB
- 每个块都有唯一ID
- 支持乱序接收和重传

## 错误处理

系统包含完善的错误处理：

- **网络错误**：自动检测连接断开
- **文件错误**：处理文件不存在、权限不足等问题
- **协议错误**：验证消息格式和数据完整性
- **资源错误**：正确释放套接字和文件句柄

## 注意事项

1. **防火墙**：确保服务器端口未被防火墙阻挡
2. **文件权限**：确保有读写文件的权限
3. **磁盘空间**：确保有足够空间接收文件
4. **网络稳定性**：断点续传功能依赖文件名识别

## Makefile

```makefile
# Makefile for TCP File Transfer System

CXX = g++
CXXFLAGS = -std=c++11 -pthread -Wall -O2
TARGETS = tcp_server tcp_client

all: $(TARGETS)

tcp_server: tcp_server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

tcp_client: tcp_client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGETS) *.o

run-server: tcp_server
	./tcp_server 8888

run-client: tcp_client
	./tcp_client 127.0.0.1 8888

test-file:
	dd if=/dev/urandom of=test_10mb.bin bs=1M count=10
	@echo "Created test_10mb.bin (10MB)"

.PHONY: all clean run-server run-client test-file
```

## 故障排除

### 问题：连接被拒绝
- 检查服务器是否正在运行
- 确认IP地址和端口号正确
- 检查防火墙设置

### 问题：文件传输中断
- 系统支持断点续传，重新发送相同文件即可
- 检查网络连接稳定性
- 确认磁盘空间充足

### 问题：传输速度慢
- 检查网络带宽
- 确认没有其他程序占用网络
- 可以调整块大小以优化性能

## 扩展功能建议

1. **加密传输**：使用SSL/TLS保护数据
2. **压缩传输**：压缩数据以减少传输量
3. **多文件传输**：支持批量文件传输
4. **带宽控制**：限制传输速度
5. **Web界面**：添加Web管理界面

## 许可证

本项目仅供学习使用。