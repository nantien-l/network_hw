/*
 編譯: g++ -o myserver myserver.cpp -lpthread
 執行: ./myserver <port> -a
 * Option:
 * -d: 顯示基本連線訊息
 * -s: 顯示連線訊息 + 線上列表
 * -a: 顯示所有詳細通訊內容 (Debug用)
 */

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <algorithm>
#include <map>
#include <iomanip>
// 先加上去 這一定不會錯....
#include <openssl/ssl.h>
#include <openssl/err.h>

using namespace std;

#define DEFAULT_BALANCE 10000

// 顯示模式 (依照作業要求)
int outputMode = 0; // 0:Default, 1:-d, 2:-s, 3:-a

struct Account {
    string name;        // 使用者名稱 (例如: "Alice")
    int balance;        // 帳戶餘額 (作業規定預設 10000)
    bool isOnline;      // 上線狀態 (true: 在線, false: 離線)
    string ip;          // Client 的 IP 位址 (例如: "127.0.0.1")
    
    // [關鍵區別]
    int port;           // 這是 Client 用來「給別人連 P2P」的 Port (例如 8888)
                        // 會顯示在 List 裡面告訴大家：「要轉帳給我請連這個 Port」

    int socketFd;       // 這是 Server 用來「傳訊息給 Client」的專屬連線代號
                        // (Socket File Descriptor)。
                        // Server 要送 "100 OK" 或 "List" 給這個人時，
                        // 必須使用 send(socketFd, ...)

    // 建構子 (初始化用)
    Account() : name(""), balance(DEFAULT_BALANCE), isOnline(false), port(0), socketFd(-1) {}
    Account(string n) : name(n), balance(DEFAULT_BALANCE), isOnline(false), port(0), socketFd(-1) {}
};

// [關鍵資料結構] 全域帳戶資料庫 (Global Database)
// Key (索引): string name -> 使用者名稱 (例如 "Alice")
// Value (內容): Account -> 對應的帳戶結構 (包含餘額、IP、Port 等)
// 用途: 當 Client 登入或轉帳時，Server 必須來這裡查資料。
map<string, Account> accounts;

// [多執行緒保護機制] 互斥鎖 (Mutex)
// 用途: 保護上面的 'accounts' 資料庫。
// 因為 Server 是多執行緒 (Multi-threaded) 的，可能同時有 A 和 B 兩個人在註冊或轉帳。
// PTHREAD_MUTEX_INITIALIZER 是靜態初始化的標準寫法。
pthread_mutex_t accountsMutex = PTHREAD_MUTEX_INITIALIZER;

// [輔助工具函式] 計算字元出現次數
// 參數 s: 要檢查的完整字串 (例如 "Alice#1000#Bob")
// 參數 c: 要找的字元 (例如 '#')
// 回傳值: 該字元出現了幾次
// 用途: 這是為了區分 Protocol 指令格式：
//       1. 登入指令 "Alice#8888" -> 只有 1 個 '#'
//       2. 轉帳指令 "Alice#1000#Bob" -> 有 2 個 '#'
int countChar(const string& s, char c) {
    int count = 0;
    // 遍歷字串中的每一個字元，如果是目標字元 c，計數器加 1
    for (char ch : s) if (ch == c) count++;
    return count;
}



// [功能說明] 顯示伺服器端內部的線上使用者列表
// 用途：這不是傳給 Client 的訊息，而是印在 Server 自己的終端機 (Console) 上供管理員查看。
// 對應作業要求：當啟動 Server 使用參數 -s 時，必須在有人登入/登出時顯示此清單 。
void printServerSideList() {
    cout << "------------------------------------------" << endl;
    cout << "Current Online List:" << endl;

    // [語法重點] C++17 Structured Binding (結構化綁定)
    // 意義：遍歷全域變數 'accounts' (這是一個 Map)。
    // accounts 的結構是 <Key, Value>，這裡我們用 [name, acc] 直接把 Key 和 Value 拆出來用。
    // name: 取得 Key (使用者名稱 string)
    // acc:  取得 Value (帳戶結構 Account)
    for (auto const& [name, acc] : accounts) {
        
        // [邏輯判斷] 只顯示目前狀態為「上線 (Online)」的使用者
        // 因為 Map 裡也會存著「已註冊但目前離線」的人，我們不需要印出那些人。
        if (acc.isOnline) {
            // [輸出格式] 名字 [Tab空格] IP位址 : P2P監聽Port
            // 例如印出: Alice    127.0.0.1:8888
            cout << name << "\t" << acc.ip << ":" << acc.port << endl;
        }
    }
    cout << "------------------------------------------" << endl;
}




// [協定實作] 產生回傳給 Client 的 "List" 訊息字串
// 參數 requestorName: 發出請求的 Client 名字 (因為我們要查他的餘額)
// 回傳值: 格式化後的字串，準備直接 send 給 Client
string generateListMessage(const string& requestorName) {
    string msg = "";
    int onlineCount = 0;
    string userList = "";

    // 1. 取得請求者的餘額 (Balance)
    // 我們需要從全域 map 'accounts' 中找到這個人的資料
    int myBalance = 0;
    if (accounts.count(requestorName)) {
        myBalance = accounts[requestorName].balance;
    }

    // 2. 遍歷全域資料庫，建立「線上使用者清單」
    for (auto const& [name, acc] : accounts) {
        // 只加入目前在線 (isOnline == true) 的使用者
        if (acc.isOnline) {
            onlineCount++; // 線上人數 +1
            
            // [關鍵格式] 建構單一使用者的資訊字串
            // 格式: 使用者名稱 # IP位址 # P2P監聽Port
            // 注意: 這裡的 acc.port 是 Client 用來 P2P 收款的 Port
            userList += acc.name + "#" + acc.ip + "#" + to_string(acc.port) + "\n";
        }
    }

    // 3. 組合最終訊息 (必須嚴格遵守作業規定的順序!)
    // Client 端是依照行數來解析的，順序錯了 Client 就會當機或讀錯資料。
    
    // 第一行: 帳戶餘額
    msg += to_string(myBalance) + "\n";      
    
    // 第二行: Server 公鑰 (目前階段先用假字串代替)
    msg += "ServerPubKey_Dummy\n";           
    
    // 第三行: 線上總人數
    msg += to_string(onlineCount) + "\n";    
    
    // 第四行以後: 所有線上使用者的詳細資料
    msg += userList;                         

    return msg;
}


// [執行緒函式] 處理單一 Client 連線的 Worker Thread
// 參數 args: void* 指標，指向主程式 (Main Thread) 傳過來的 Socket Descriptor
// 回傳值: NULL (因為我們是用 detach 模式，不需要回傳值)
void* clientHandler(void* clientSocketPtr) {
    
    // [記憶體管理] 
    // 1. 將 void* 指標強制轉型回 int* 指標，再取值 (*)，拿到真正的 Socket ID (例如: 4)
    int clientSock = *(int*)clientSocketPtr;
    
    // 2. [關鍵] 釋放記憶體
    // 主程式在 accept 後使用了 'new int' 配置記憶體來傳遞 socket。
    // 如果這裡不 free，每有一個人連線就會洩漏 4 bytes 記憶體 (Memory Leak)。
    free(clientSocketPtr); 

    // [緩衝區準備]
    // 準備一個 4096 bytes 的大陣列來暫存收到的資料
    // 網路封包通常不會超過這個大小 (MTU 通常 1500)，所以 4096 很安全。
    char buffer[4096];
    
    // 用來記錄「現在這個連線是誰登入的」
    // 初始為空字串，等到收到 Login 指令後才會更新
    string currentUserName = ""; 

    // [無窮迴圈] 持續服務這個 Client
    // 只要 Client 沒斷線，這個 Thread 就會一直跑在這裡
    while (true) {
        
        // [清空緩衝區]
        // 每次接收新資料前，先把 buffer 全部填 0。
        // 防止上一次的殘留資料汙染這一次的讀取。
        memset(buffer, 0, sizeof(buffer));
        
        // [接收資料 - Blocking Call]
        // 呼叫 recv 系統呼叫 (System Call) 從 socket 讀取資料。
        // * 重要觀念: 這是「阻塞式 I/O」。如果 Client 沒傳資料，程式會「卡」在這裡等待。
        // 參數說明: 
        // 1. clientSock: 從哪個連線讀
        // 2. buffer: 讀到的資料放哪
        // 3. sizeof-1: 預留最後一個 byte 給字串結尾符號 '\0'
        // 4. 0: 預設 flag
        int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        
        // [斷線判斷]
        // 如果回傳值 <= 0，代表發生狀況：
        // 0: Client 正常關閉連線 (Called close())
        // -1: 發生錯誤 (Error)
        if (bytesReceived <= 0) {
            
            // 如果這個連線已經登入過 (currentUserName 有值)，我們要幫他「登出」
            if (!currentUserName.empty()) {
                
                // [執行緒安全 - 上鎖]
                // 因為我們要修改全域變數 'accounts'，必須先鎖住，
                // 避免此時有另一個 Thread 剛好在轉帳給這個人，造成資料錯亂 (Race Condition)。
                pthread_mutex_lock(&accountsMutex);
                
                // 修改狀態：設為離線
                accounts[currentUserName].isOnline = false; 
                
                // [作業要求 -d] 顯示離線訊息
                if (outputMode >= 1) {
                    cout << "[Info] User " << currentUserName << " disconnected." << endl;
                }
                // [作業要求 -s] 離線後顯示最新列表 (方便助教檢查)
                if (outputMode >= 2) {
                    printServerSideList();
                }
                
                // [執行緒安全 - 解鎖]
                // 修改完畢，解鎖讓其他人可以用
                pthread_mutex_unlock(&accountsMutex);
            }
            
            // [資源回收] 
            // 關閉 Socket，釋放作業系統的 File Descriptor 資源
            close(clientSock); 
            
            // 跳出 while 迴圈 -> 函式結束 -> Thread 結束
            break; 
        }

        // [資料處理]
        // 將 char 陣列轉換成 C++ 的 string 物件，方便後續處理
        string cmd(buffer);
        
        // [字串正規化 - 去除換行]
        // Client (特別是手動輸入或測試程式) 送來的字串尾端常會有隱藏的 '\n' 或 '\r'。
        // 例如 "List\n" 跟 "List" 是不一樣的。
        // 這裡使用 C++ STL 演算法 remove 將這些不可見字元刪除，確保指令比對精確。
        cmd.erase(std::remove(cmd.begin(), cmd.end(), '\n'), cmd.end());
        cmd.erase(std::remove(cmd.begin(), cmd.end(), '\r'), cmd.end());

        // 如果刪完發現是空字串 (例如 Client 只按了 Enter)，就忽略這次請求
        if (cmd.empty()) continue;

        // [作業要求 -a] Debug 模式
        // 顯示所有接收到的原始指令，證明 Server 有收到東西
        if (outputMode >= 3) {
            cout << "[Recv from " << (currentUserName.empty() ? "Unknown" : currentUserName) << "]: " << cmd << endl;
        }

        // 準備一個字串用來存放要回傳給 Client 的訊息
        string response = "";

        // [執行緒安全 - 進入核心邏輯區]
        // 接下來的判斷都會讀取或修改全域 map 'accounts'，所以必須全程上鎖
        pthread_mutex_lock(&accountsMutex);

        // ==========================================================
        // 指令 1. 註冊 (REGISTER)
        // 邏輯: 判斷字串是否以 "REGISTER#" 開頭
        // ==========================================================
        if (cmd.find("REGISTER#") == 0) {
            // 切割字串: 從第 9 個字元開始切 (跳過 "REGISTER#") 拿到名字
            string name = cmd.substr(9); 
            
            // 檢查 map 中是否已經有這個 key
            if (accounts.count(name)) {
                // 有 -> 註冊失敗
                response = "210 FAIL\n"; 
            } else {
                // 無 -> 建立新帳號
                // 呼叫 Account 建構子，預設餘額 10000
                accounts[name] = Account(name);
                response = "100 OK\n";
                
                // [作業要求 -d] 在 Server 端印出 Log
                if (outputMode >= 1) cout << "[Register] New user: " << name << endl;
            }
        }
        
        // ==========================================================
        // 指令 2. 登入 (Login)
        // 格式: Name#Port
        // 邏輯: 根據 Client 行為，登入指令只有 1 個 '#'
        // ==========================================================
        else if (countChar(cmd, '#') == 1) {
            // 找 '#' 的位置
            size_t pos = cmd.find('#');
            
            // 切割前半段: 名字
            string name = cmd.substr(0, pos);
            // 切割後半段: Port (還是字串)
            string portStr = cmd.substr(pos + 1);
            // 將 Port 字串轉成整數 (atoi: ASCII to Integer)
            int port = atoi(portStr.c_str());

            // 檢查帳號是否存在
            if (accounts.count(name) == 0) {
                // 沒註冊過不能登入 -> 回傳驗證失敗
                response = "220 AUTH_FAIL\n";
            } else {
                // [網路程式設計 - 取得對方 IP]
                // getpeername 是一個系統呼叫，可以查出目前這個 socket 連線的對方是誰
                struct sockaddr_in addr;
                socklen_t addr_size = sizeof(struct sockaddr_in);
                getpeername(clientSock, (struct sockaddr *)&addr, &addr_size);
                
                // 更新該使用者的狀態
                accounts[name].isOnline = true; // 設為上線
                accounts[name].ip = inet_ntoa(addr.sin_addr); // 將 binary IP 轉成字串 (例如 127.0.0.1)
                accounts[name].port = port; // 儲存 Client 的 P2P Port (很重要！這是給別人連的)
                accounts[name].socketFd = clientSock; // 綁定目前的連線 ID
                
                currentUserName = name; // 讓這個 Thread 記住現在服務的是誰

                // [作業要求 -d] 印出登入 Log
                if (outputMode >= 1) cout << "[Login] User: " << name << " on port " << port << endl;
                // [作業要求 -s] 登入成功後，印出目前所有線上名單
                if (outputMode >= 2) printServerSideList();

                // [關鍵] 登入成功後，要回傳「帳戶資訊 + 線上列表」給 Client
                response = generateListMessage(currentUserName);
            }
        }
        
        // ==========================================================
        // 指令 3. 查詢列表 (List)
        // 格式: List
        // ==========================================================
        else if (cmd == "List") {
            // 防呆: 如果沒登入就想查 List
            if (currentUserName.empty()) {
                 response = "Please login first\n";
            } else {
                 // 呼叫輔助函式，產生符合協定格式的列表字串
                 response = generateListMessage(currentUserName);
            }
        }
        
        // ==========================================================
        // 指令 4. 離開 (Exit)
        // 格式: Exit
        // ==========================================================
        else if (cmd == "Exit") {
            // 如果目前是登入狀態，要設為離線
            if (!currentUserName.empty()) {
                accounts[currentUserName].isOnline = false;
                
                // [作業要求 -d] 印出 Log
                if (outputMode >= 1) cout << "[Info] User " << currentUserName << " disconnected." << endl;
                // [作業要求 -s] 印出剩餘線上名單
                if (outputMode >= 2) printServerSideList();
            }
            response = "Bye\n";
            
            // 這裡直接送出 Bye，然後下面會 break 跳出迴圈
            send(clientSock, response.c_str(), response.length(), 0);
            
            // [重要] 因為要 break 了，必須在這裡解鎖，不然 Mutex 會永遠被鎖住 (Deadlock)
            pthread_mutex_unlock(&accountsMutex); 
            
            close(clientSock); // 關閉連線
            break; // 跳出 while 迴圈
        }
        
        // ==========================================================
        // 指令 5. 轉帳通知 (Transaction)
        // 格式: Sender#Amount#Receiver
        // 邏輯: 轉帳指令會有 2 個 '#' (這是根據作業觀察出來的特徵)
        // ==========================================================
        else if (countChar(cmd, '#') == 2) {
            // 使用 stringstream 來切割字串 (C++ 解析字串的好工具)
            stringstream ss(cmd);
            string sender, amountStr, receiver;
            
            // 依序讀取，用 '#' 當作分隔符號
            getline(ss, sender, '#');
            getline(ss, amountStr, '#');
            getline(ss, receiver, '#');
            
            // 將金額轉成整數
            int amount = atoi(amountStr.c_str());

            // [邏輯檢查] 確認轉出者和接收者都存在於資料庫中
            if (accounts.count(sender) && accounts.count(receiver)) {
                
                // [核心轉帳邏輯] 直接操作記憶體中的餘額
                accounts[sender].balance -= amount;   // 扣錢
                accounts[receiver].balance += amount; // 加錢
                
                // 回傳 OK，讓 Client 知道 Server 已經記帳了
                response = "Transfer OK\n";
                
                // [作業要求 -d] 轉帳是很重要的操作，必須記錄
                if (outputMode >= 1) cout << "[Transfer] " << sender << " -> " << receiver << " (" << amount << ")" << endl;
            } else {
                // 如果找不到人
                response = "Transfer Fail\n";
            }
        }
        
        // ==========================================================
        // 未知指令 (Error Handling)
        // ==========================================================
        else {
            response = "230 Input format error\n";
        }

        // [執行緒安全 - 解鎖]
        // 所有 map 操作完成，釋放鎖，讓其他 Thread 可以工作
        pthread_mutex_unlock(&accountsMutex); 

        // [發送回應]
        // 如果 response 不是空的，就透過 socket 傳回給 Client
        if (!response.empty()) {
            send(clientSock, response.c_str(), response.length(), 0);
            
            // [作業要求 -a] 顯示送出去的內容 (Debug)
            if (outputMode >= 3) {
                 string logRes = response;
                 // 移除換行以免 Log 在終端機上太亂
                 logRes.erase(std::remove(logRes.begin(), logRes.end(), '\n'), logRes.end());
                 cout << "[Send to " << currentUserName << "]: " << logRes << endl;
            }
        }
    }
    return NULL;
}





// argc: 參數個數 (Argument Count)
// argv: 參數內容陣列 (Argument Vector)，例如 ./server 8888 -a
int main(int argc, char *argv[]) {


    // 初始化 OpenSSL 函式庫 （新步驟）
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();   
    
    // [參數檢查] 
    // 規定 Server 啟動指令格式為: ./server <port> [Option]
    // 所以參數數量必須是 2 (只有port) 或 3 (有port也有option)
    if (argc < 2 || argc > 3) {
        cout << "Usage: ./server <port> [Option]" << endl;
        cout << "Option:" << endl;
        cout << "  -d: Basic messages (Register/Login/Exit)" << endl;
        cout << "  -s: Also show online list" << endl;
        cout << "  -a: Show all message transfers (Debug)" << endl;
        return 0; // 參數錯誤，直接結束程式
    }

    // 將第二個參數 (argv[1]) 從字串轉成整數，這就是我們要監聽的 Port (例如 8888)
    int port = atoi(argv[1]);

    // [參數解析] 處理作業規定的額外功能 (-d, -s, -a) [cite: 104-110]
    if (argc == 3) {
        string opt = argv[2]; // 取得第三個參數
        if (opt == "-d") outputMode = 1;      // 設定全域變數 outputMode
        else if (opt == "-s") outputMode = 2; 
        else if (opt == "-a") outputMode = 3; 
        else {
            cout << "Unknown option: " << opt << endl;
            return 0;
        }
    }

    // ==========================================
    // 步驟 1: 建立 Socket 
    // ==========================================
    // AF_INET: 使用 IPv4 協定
    // SOCK_STREAM: 使用 TCP 協定 (可靠、連線導向)
    // 0: 自動選擇對應協定
    // 回傳值: serverSocket 是一個 File Descriptor (整數 ID)，代表這支「總機電話」
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    
    // [地址結構設定] 告訴系統這支電話要綁定在哪裡
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET; // IPv4
    
    // INADDR_ANY: 綁定本機所有網卡 (All Interfaces)。
    // 意思是不管 Client 是從 Wi-Fi 連進來、還是從網路線連進來，Server 都收。
    serverAddr.sin_addr.s_addr = INADDR_ANY; 
    
    // htons (Host to Network Short): 
    // 將 Port 從「電腦看不懂的格式 (Little Endian)」轉成「網路標準格式 (Big Endian)」。
    // 這是網路程式設計的必備動作！
    serverAddr.sin_port = htons(port);

    // [Socket 選項設定] 允許 Port 重複使用
    // 用途: 如果你強制關閉 Server (Ctrl+C)，Port 常常會被系統鎖住一段時間。
    // 這行設定讓你可以馬上重開 Server，不用等系統釋放 Port。
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ==========================================
    // 步驟 2: 綁定 (Bind) (插入 SIM 卡，設定電話號碼)
    // ==========================================
    // 將 serverSocket (手機) 與 serverAddr (號碼/Port) 綁定在一起。
    // 如果 Port 已經被別人用了 (例如另一個 Server)，這裡會回傳 < 0 (失敗)。
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed"); // 印出錯誤原因
        return -1;
    }

    // ==========================================
    // 步驟 3: 監聽 (Listen) (開機，準備接電話)
    // ==========================================
    // 參數 10 (Backlog): 指定「同時等待接聽」的佇列長度。
    // 如果 Server 很忙還沒 accept，最多允許 10 個人在線上排隊，第 11 個會被拒絕。
    listen(serverSocket, 10);


    // ================================
    // TLS / OpenSSL Context 初始化
    // ================================

    // 建立一個 TLS Server 的 Context（設定藍圖）
    SSL_CTX* sslCtx = SSL_CTX_new(TLS_server_method());
    if (!sslCtx) {
        ERR_print_errors_fp(stderr);
        return -1;
    }
    
    // 載入 Server Certificate（公鑰 + 身分資訊）
    if (SSL_CTX_use_certificate_file(sslCtx, "mycert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        return -1;
    }

    // 載入 Server Private Key
    if (SSL_CTX_use_PrivateKey_file(sslCtx, "mykey.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        return -1;
    }

    // 檢查 Private Key 是否與 Certificate 相符
    if (!SSL_CTX_check_private_key(sslCtx)) {
        cerr << "Private key does not match the certificate!" << endl;
        return -1;
    }

    // 顯示啟動成功的訊息與目前的模式
    cout << "Server started on port " << port;
    if (outputMode == 1) cout << " (Mode: -d)";
    else if (outputMode == 2) cout << " (Mode: -s)";
    else if (outputMode == 3) cout << " (Mode: -a)";
    cout << endl;

    // ==========================================
    // 步驟 4: 接受連線迴圈 (Accept Loop) (總機櫃檯)
    // ==========================================
    while (true) {
        // 準備用來存放 Client 資訊 (IP, Port) 的結構
        sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        
        // [關鍵 - Blocking Call] 接受連線
        // accept 會卡在這裡等待，直到有 Client 連上來。
        // 回傳值 clientSock: **這是一個全新的 Socket ID** (例如 4號)。
        // * serverSocket (總機) 繼續留在這裡等下一個人。
        // * clientSock (分機) 專門用來跟這個剛連上來的 Client 講話。
        int clientSock = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

        // 如果連線失敗 (例如被訊號中斷)，就忽略，繼續等下一個
        if (clientSock < 0) continue;

        // [記憶體配置] 為執行緒準備參數
        // 因為 pthread_create 需要傳指標，我們不能直接傳區域變數的位址 (會有 Race Condition)。
        // 所以用 new 配置一塊新的記憶體，把 clientSock 的值抄進去。
        // 這塊記憶體會由 clientHandler 負責 delete (free)。
        int* newSock = new int;
        *newSock = clientSock;

        // [多執行緒 - 派工] 建立 Worker Thread
        // 參數說明:
        // 1. &tid: 存放新執行緒 ID 的變數
        // 2. NULL: 預設屬性
        // 3. clientHandler: 新執行緒要跑的函式 
        // 4. (void*)newSock: 傳給函式的參數 (也就是剛剛拿到的分機號碼)
        pthread_t tid;
        pthread_create(&tid, NULL, clientHandler, (void*)newSock);
        
        // [分離執行緒] Fire and Forget
        // 告訴系統：「這個執行緒跑完自己收屍就好，主程式不會在那邊等它 (join)」。
        // 這是 Server 程式的常見寫法，避免主程式被卡住。
        pthread_detach(tid); 
    }
    
    // 理論上 while(true) 不會執行到這裡，除非有 break
    return 0;
}