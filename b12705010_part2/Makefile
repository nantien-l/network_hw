# 定義編譯器
CC = gcc
CXX = g++

# 定義編譯參數 (-Wall: 顯示警告, -O2: 優化, -pthread: 支援多執行緒)
CFLAGS = -Wall -Wextra -O2
CXXFLAGS = -Wall -Wextra -O2 -pthread -std=c++17

# 目標：輸入 make 時預設會編譯這兩個
all: client server

# 編譯 Client
client: client.c
	$(CC) $(CFLAGS) -o client client.c

# 編譯 Server 
server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

# 清除指令 (輸入 make clean 會刪除執行檔)
clean:
	rm -f client server