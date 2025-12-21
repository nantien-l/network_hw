

// client.c HW1 Part1 minimal client
// 編譯: gcc client.c -o client -lssl -lcrypto -lpthread
// 執行: ./client <server_ip> <server_port>

#include <stdio.h>          // printf, fprintf, perror, fgets
#include <stdlib.h>         // atoi, exit
#include <string.h>         // memset, strlen, strcspn, strcasecmp
#include <unistd.h>         // close, read/write
#include <arpa/inet.h>      // inet_pton, htons, struct sockaddr_in
#include <sys/socket.h>     // socket, connect, send, recv
#include <sys/select.h>     // select() 聚合收資料
#include <errno.h>          // errno
#include <openssl/ssl.h>
#include <openssl/err.h>

// ---- 統一緩衝區大小：4KB ----
#define BUFSZ 4096
#define MAX_USERNAME 128
#define MAX_ONLINE_USERS 100

// 線上使用者資訊結構
typedef struct {
    char username[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int port;
} OnlineUser;

// 全域變數
int listen_sd = -1;                     // 本地監聽 socket
char my_name[MAX_USERNAME] = "";        // 自己的名稱
int my_port = 0;                        // 自己的 port
OnlineUser online_users[MAX_ONLINE_USERS];
int num_online_users = 0;

// 已實作函式一覽 (function prototypes)
static void rstrip(char *s);                                                   //移除字串 s 尾端的換行或空白字元（就地修改），避免末尾殘留 '\n'、'\r' 或空格。
static void drain_server_ack(int server_sd, int max_wait_ms);                  //從 server_sd 讀走（丟棄）短期內的回應/ack，最多等待 max_wait_ms 毫秒，用來清空暫存回應。           
static int connect_tcp(const char* ip, int port);                              //建立一個 TCP 連線到指定 ip:port，成功回傳 socket fd，失敗回傳 -1。
static int connect_peer(const char* ip, int port);                             //建立到 peer（另一個 client）的 TCP 連線，通常用於 P2P 傳輸；成功回傳 socket fd，失敗回傳 -1。  
static int create_listen_socket(int port);                                     //建立、綁定並 listen 在指定 port 的 TCP listening socket，回傳 listening socket fd，失敗回傳 -1。
static OnlineUser* find_online_user(const char* username);                     //在本地維護的 OnlineUser 清單中搜尋 username，若找到回傳指標，否則回傳 NULL。
static void send_line(int sd, const char* msg);                                //將 msg 透過非 TLS 的 socket sd 傳送出去，確保整行送出。
static void send_line_tls(SSL* ssl, const char* msg);                          //將 msg 透過 TLS/SSL 連線傳送出去，確保整行送出並處理 SSL 錯誤。
static int recv_full_burst(int sd, char *out, int out_sz);                     //從 socket sd 讀取可用的資料塊直到沒有資料或緩衝滿為止，將資料寫入 out，回傳讀到的位元組數。
static void recv_print_aggregate(int sd);                                      //從非 TLS socket sd 收集並組合一段可用資料，並將接收到的訊息打印/處理（例如顯示在終端）。
static void recv_print_aggregate_tls(SSL *ssl);                                //從 TLS/SSL 連線讀取並組合可用資料，將接收到的訊息打印/處理（SSL 版本的 aggregate 接收）。
static void update_online_users(const char* response);                         //解析伺服器回傳的線上使用者列表 response，更新本地的 OnlineUser 清單（新增/移除/更新狀態）。
static void handle_p2p_transfer(const char* receiver, int amount, int server_sd);       //發起對 receiver 的 P2P 轉帳流程：可能先通知伺服器取得對方資訊，建立 P2P 連線並傳送金額等資料。
static void handle_incoming_p2p(int p2p_sd, int server_sd);                     //處理來自其他 peer 的傳入 P2P 連線（由 p2p_sd 接受），接收傳輸資料並視情況向伺服器回報/確認。

//------------------------------------------------------------------------------
// 工具：把字串尾端所有  "\r"、"\n"、" "、"\t" 砍掉
// 目的：確保送出的訊息沒有多餘尾巴
//------------------------------------------------------------------------------
static void rstrip(char *s) {
    if (!s) return;
    // strcspn: 回傳字串 s 中「第一次遇到集合內任一字元」的位置
    // 這裡集合為 "\r\n\t "，所以可一次去掉常見尾端空白與換行。
    s[strcspn(s, "\r\n\t ")] = '\0';
}

//------------------------------------------------------------------------------
// 讀掉 server 當前可讀的所有資料（避免黏到下一個指令的回覆）
// max_wait_ms: 最多等這麼久（毫秒）
// ------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// 函式名稱：drain_server_ack
// 功能：快速清空伺服器傳來但尚未讀取的 ACK 或雜訊資料
// 用途：防止前一個回覆（例如 Transfer OK!）黏在下一個指令的開頭
//
// 特性：
//   - 最多等 100ms，單輪等 20ms
//   - 非阻塞 recv()
//   - 若 debug_mode = 1，會顯示清掉的內容
//------------------------------------------------------------------------------
static void drain_server_ack(int server_sd, int max_wait_ms) {
    fd_set rfds;
    struct timeval tv;
    char buf[BUFSZ];

    int remaining_ms = (max_wait_ms > 0) ? max_wait_ms : 100;  // 預設最多 100ms

    while (remaining_ms > 0) {
        FD_ZERO(&rfds);
        FD_SET(server_sd, &rfds);

        tv.tv_sec  = 0;
        tv.tv_usec = 20 * 1000; // 每輪最多等 20ms

        int ready = select(server_sd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) break;            // timeout 或錯誤
        if (!FD_ISSET(server_sd, &rfds)) break;

        int n = recv(server_sd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n <= 0) break;

        buf[n] = '\0';
        

        remaining_ms -= 20;
    }
}

//------------------------------------------------------------------------------
// 連線：建立 TCP socket 並 connect 到 <ip:port>
// 失敗就直接印錯並結束程式。
//------------------------------------------------------------------------------
static int connect_tcp(const char* ip, int port) {
    int sd = socket(AF_INET, SOCK_STREAM, 0);  // AF_INET=IPv4, SOCK_STREAM=TCP
    if (sd < 0) { 
        perror("socket"); 
        exit(1); 
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));           // 清 0，避免未初始化的雜訊
    addr.sin_family = AF_INET;                 // 指定 IPv4
    addr.sin_port   = htons(port);             // 本機端序 -> 網路端序(big endian)

    // 將字串 IP（例如 "127.0.0.1"）轉成二進位放入 addr.sin_addr
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "FAIL!! inet_pton failed for %s\n", ip);
        exit(1);
    }

    // 主動建立 TCP 連線（三向握手在內核完成）
    if (connect(sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    return sd; // 回傳可用的連線 socket
}


// 專給 P2P 用：連不上只回傳 -1，不要 exit(1)
static int connect_peer(const char* ip, int port) {
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) { perror("socket(peer)"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton(peer) failed for %s\n", ip);
        close(sd);
        return -1;
    }
    if (connect(sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect(peer)");
        close(sd);
        return -1;
    }
    return sd;
}



//------------------------------------------------------------------------------
// 建立 P2P 監聽 socket（讓別人能連進來轉帳）
//------------------------------------------------------------------------------
static int create_listen_socket(int port) {
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sd);
        exit(1);
    }

    if (listen(sd, 5) < 0) {
        perror("listen");
        close(sd);
        exit(1);
    }

    printf("[INFO] Listening on port %d for P2P transfers\n", port);
    return sd;
}

//------------------------------------------------------------------------------
// 根據使用者名稱尋找線上使用者
//------------------------------------------------------------------------------
static OnlineUser* find_online_user(const char* username) {
    for (int i = 0; i < num_online_users; i++) {
        if (strcmp(online_users[i].username, username) == 0) {
            return &online_users[i];
        }
    }
    return NULL;
}

//------------------------------------------------------------------------------
// 送資料
//------------------------------------------------------------------------------
static void send_line(int sd, const char* msg) {
    char buf[BUFSZ];                          // 暫存組好的訊息
    // snprintf: 安全地將字串格式化到緩衝區
    int len = snprintf(buf, sizeof(buf), "%s", msg);

    // 直接呼叫 send() 一次送出去
    ssize_t n = send(sd, buf, len, 0);

    // 基本安全檢查：若送出異常
    if (n < 0) {
        perror("send");                       // 印錯誤原因
        exit(1);                              // 結束程式
    }
    
    // 如果發生 partial send (n < len)，這個函式沒有保證送完，
    // 但因為訊息很短，實務上通常不會有問題。
}

//------------------------------------------------------------------------------
// 送資料（加密版，for TLS）
//------------------------------------------------------------------------------
static void send_line_tls(SSL* ssl, const char* msg) {
    char buf[BUFSZ];
    int len = snprintf(buf, sizeof(buf), "%s", msg);

    int n = SSL_write(ssl, buf, len);
    if (n <= 0) {
        fprintf(stderr, "[TLS] SSL_write failed.\n");
        ERR_print_errors_fp(stderr);
    }
}



//------------------------------------------------------------------------------
// 簡化版 recv_full_burst：只收一次，不等延續包
//------------------------------------------------------------------------------
static int recv_full_burst(int sd, char *out, int out_sz) {
    int n = recv(sd, out, out_sz - 1, 0);
    if (n <= 0) return n;
    out[n] = '\0';
    return n;
}

//------------------------------------------------------------------------------
// 收資料並印出（小聚合版）：
// 1) 先做一次阻塞 recv()，保證至少拿到第一批回覆。
// 2) 接著用 select() 給個很短的 timeout（例如 150ms），
//    只要還有資料可讀就繼續 recv，盡量把「同一波回覆」湊齊再印。
//------------------------------------------------------------------------------
static void recv_print_aggregate(int sd) {
    char buf[BUFSZ];
    int total = 0;

    // 第一次阻塞讀：等 server 回覆
    int n = recv(sd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return;       // n==0：對端關閉；n<0：錯誤
    buf[n] = '\0';
    fputs(buf, stdout);
    total += n;

    // 之後用 select() 小等候把餘波收齊
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sd, &rfds);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 30 * 1000; // 150ms：太短看不到字，太長變得很慢

        int ready = select(sd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) break;           // 0: 超時(沒新資料)；<0: 錯誤→結束聚合
        if (!FD_ISSET(sd, &rfds)) break; // 理論上不會發生

        n = recv(sd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;               // 沒資料或對端關閉
        buf[n] = '\0';
        fputs(buf, stdout);
        total += n;
        if (total >= BUFSZ - 1) break;   // 防止一次輸出過大
    }
}

//------------------------------------------------------------------------------
// 收資料並印出（小聚合版，TLS 版）：專門對付 client to client 
// P2P 傳輸時的 SSL_read 聚合
//------------------------------------------------------------------------------

static void recv_print_aggregate_tls(SSL *ssl) {
    char buf[BUFSZ];
    int total = 0;

    while (1) {
        int n = SSL_read(ssl, buf, sizeof(buf) - 1);
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // 暫時沒資料，稍微休息一下再試
                usleep(50 * 1000); 
                continue;
            }
            break; // 其他錯誤或連線結束
        }
        buf[n] = '\0';
        fputs(buf, stdout);
        total += n;
        if (total >= BUFSZ - 1) break;
    }
}



//------------------------------------------------------------------------------
// 更新線上使用者清單
//------------------------------------------------------------------------------
static void update_online_users(const char* response) {
    num_online_users = 0;

    char buf[BUFSZ];
    strncpy(buf, response, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // 跳過前面非數字開頭的雜訊（例如 "Transfer OK!" 黏上來）
    char *start = buf;
    while (*start && (*start < '0' || *start > '9')) {
        char *nl = strchr(start, '\n');
        if (!nl) break;
        start = nl + 1;
    }
    if (!*start) {
        // 找不到以數字開頭的行，放棄解析
        printf("[WARN] Cannot find start of List payload. Raw:\n%s\n", buf);
        return;
    }

    // 從 start 開始逐行解析
    int line_no = 0;
    int expected_users = 0;

    // 我們不能直接 strtok(buf, ...) 了，因為要從 start 開始
    char *saveptr = NULL;
    char *line = strtok_r(start, "\n", &saveptr);

    while (line) {
        // 去掉 \r
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        if (line_no == 0) {
            printf("Balance: %s\n", line);
        } else if (line_no == 1) {
            printf("ServerKey: %s\n", line);
        } else if (line_no == 2) {
            expected_users = atoi(line);
        } else if (line_no >= 3 && num_online_users < MAX_ONLINE_USERS) {
            char name[64], ip[64];
            int port;
            if (sscanf(line, "%[^#]#%[^#]#%d", name, ip, &port) == 3) {
                strncpy(online_users[num_online_users].username, name,
                        sizeof(online_users[num_online_users].username) - 1);
                online_users[num_online_users].username[sizeof(online_users[num_online_users].username) - 1] = '\0';
                strncpy(online_users[num_online_users].ip, ip,
                        sizeof(online_users[num_online_users].ip) - 1);
                online_users[num_online_users].ip[sizeof(online_users[num_online_users].ip) - 1] = '\0';
                online_users[num_online_users].port = port;
                num_online_users++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
        line_no++;
        if (expected_users && num_online_users >= expected_users) {
            // 解析夠了，可提前結束
            break;
        }
    }

    printf("[INFO] Online users list updated (%d users):\n", num_online_users);
    for (int i = 0; i < num_online_users; i++) {
        printf("  - %s@%s:%d\n",
               online_users[i].username,
               online_users[i].ip,
               online_users[i].port);
    }
}

//------------------------------------------------------------------------------
// P2P 轉帳（加密版）
//------------------------------------------------------------------------------
static void handle_p2p_transfer(const char* receiver, int amount, int server_sd) {
    OnlineUser* target = find_online_user(receiver);
    if (!target) {
        printf("[WARN] Receiver '%s' not found or not online.\n", receiver);
        return;
    }

    // === 1️⃣ 建立 TCP 連線 ===
    int peer_sd = connect_peer(target->ip, target->port);
    if (peer_sd < 0) {
        printf("[WARN] Cannot connect to %s@%s:%d\n", receiver, target->ip, target->port);
        return;
    }

    // === 2️⃣ 初始化 SSL Context ===
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        close(peer_sd);
        return;
    }

    // === 3️⃣ 建立 SSL 物件並綁定 socket ===
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, peer_sd);

    // === 4️⃣ 執行 TLS 握手 ===
    if (SSL_connect(ssl) <= 0) {
        fprintf(stderr, "❌ SSL handshake with %s failed!\n", receiver);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(peer_sd);
        return;
    }

    printf("[SSL] Connected securely to %s@%s:%d\n", receiver, target->ip, target->port);

    // === 5️⃣ 傳送加密訊息 ===
    char msg[BUFSZ];
    snprintf(msg, sizeof(msg), "%s#%d#%s", my_name, amount, receiver);
    if (SSL_write(ssl, msg, strlen(msg)) <= 0) {
        fprintf(stderr, "❌ SSL_write failed.\n");
        ERR_print_errors_fp(stderr);
    } else {
        printf("[INFO] Sent encrypted P2P transfer: %s → %s (%d)\n", my_name, receiver, amount);
    }

    // === 6️⃣ 結束 TLS 連線並清理 ===
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(peer_sd);

    // === 7️⃣ 向伺服器更新餘額 ===
    drain_server_ack(server_sd, 200);
    usleep(200000);
    send_line(server_sd, "List");

    char resp[BUFSZ];
    int nlist = recv_full_burst(server_sd, resp, sizeof(resp));
    if (nlist > 0) {
        printf("%s", resp);
        update_online_users(resp);
    }
    printf("[INFO] Balance refreshed.\n");
}

//------------------------------------------------------------------------------
// 🔒 P2P 接收端（加密版）
//------------------------------------------------------------------------------
static void handle_incoming_p2p(int p2p_sd, int server_sd) {
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int conn = accept(p2p_sd, (struct sockaddr*)&cli, &len);
    if (conn < 0) { perror("accept"); return; }

    // === 1️⃣ 建立 SSL context ===
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        close(conn);
        return;
    }

    // === 2️⃣ 載入伺服端憑證與金鑰 ===
    // 這裡假設你在同目錄下有 server.pem（自簽用）
    if (SSL_CTX_use_certificate_file(ctx, "server.pem", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "server.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        close(conn);
        return;
    }

    // === 3️⃣ 建立 SSL 並綁定連線 ===
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, conn);

    // === 4️⃣ 執行 TLS 握手 ===
    if (SSL_accept(ssl) <= 0) {
        fprintf(stderr, "❌ SSL handshake (incoming) failed!\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(conn);
        return;
    }

    printf("[SSL] Secure P2P connection established with peer.\n");

    // === 5️⃣ 收資料（解密後內容） ===
    char buf[BUFSZ];
    int n = SSL_read(ssl, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "[SSL] No data received or connection closed.\n");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(conn);
        return;
    }

    buf[n] = '\0';
    printf("[P2P] Received transfer message: %s\n", buf);

    char sender[64], receiver[64];
    int amount = 0;

    if (sscanf(buf, "%[^#]#%d#%s", sender, &amount, receiver) != 3) {
        printf("[P2P] Invalid transfer format received: %s\n", buf);
    } else {
        printf("[P2P] %s sent you %d\n", sender, amount);

        // --- Step 1. 通知 Server 更新帳務 ---
        send_line(server_sd, buf);

        // --- Step 2. 等 Server 回 Transfer OK! ---
        char ack[BUFSZ];
        int n_ack = recv_full_burst(server_sd, ack, sizeof(ack));
        if (n_ack > 0) {
            ack[n_ack] = '\0';
            printf("[SERVER ACK] %s", ack);
        }

        // --- Step 3. 要 List（刷新餘額） ---
        send_line(server_sd, "List");
        char list_resp[BUFSZ];
        int n_list = recv_full_burst(server_sd, list_resp, sizeof(list_resp));
        if (n_list > 0) {
            list_resp[n_list] = '\0';
            printf("%s", list_resp);
            update_online_users(list_resp);
        }
    }

    // === 6️⃣ 關閉連線 ===
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(conn);
}

//------------------------------------------------------------------------------
// 主程式：互動式 loop
// 使用方式：./client <server_ip> <server_port>
//------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    // 1️⃣ 建立 TCP 連線
    int sd = connect_tcp(ip, port);
    printf("CONNECTION SUCCESSFUL!! Connected to %s:%d\n", ip, port);

    char line[BUFSZ];
    char recvbuf[BUFSZ];

    while (1) {
        // 顯示可用指令提示
        printf("Commands: REGISTER#NAME, NAME#4444, List, USERA#100#USERB, Exit\n");
        printf("> ");

        if (!fgets(line, sizeof(line), stdin)) break;
        rstrip(line);
        if (!*line) continue;

        // ------------------------------------------------------------------
        // 🟡 A. 檢查是否是「轉帳指令」格式： USERA#100#USERB
        // ------------------------------------------------------------------
        char sender[64], receiver[64];
        int amount;
        if (sscanf(line, "%[^#]#%d#%s", sender, &amount, receiver) == 3) {
            printf("[LOCAL] Detected transfer command: %s → %s (%d)\n",
                   sender, receiver, amount);
            handle_p2p_transfer(receiver, amount, sd);
            continue;
        }

        // ------------------------------------------------------------------
        // 🟢 B. 若輸入的是 NAME#PORT（登入）→ 開啟 P2P 監聽
        // ------------------------------------------------------------------
        if (strchr(line, '#') && strncasecmp(line, "REGISTER#", 9) != 0) {
            char* sharp = strchr(line, '#');
            int portnum = atoi(sharp + 1);
            strncpy(my_name, line, sharp - line);
            my_name[sharp - line] = '\0';
            my_port = portnum;
            listen_sd = create_listen_socket(portnum);
            printf("[INFO] Listening on port %d for P2P transfers.\n", portnum);
        }

        // ------------------------------------------------------------------
        // 🟣 C. 傳送指令給伺服器
        // ------------------------------------------------------------------
        send_line(sd, line);

        // 嘗試一次收完整回覆
        // 用「收一整波」確保不黏下一次回覆
        memset(recvbuf, 0, sizeof(recvbuf));
        int n = recv_full_burst(sd, recvbuf, sizeof(recvbuf));
        if (n > 0) {
            printf("%s", recvbuf);

            // 只有當我們**確定**回覆是 List 內容，才去 parse
            // 條件 1：剛剛就是送 "List"
            // 條件 2：或是登入（name#port）時 server 會回傳 List 格式
            // 另外再加一層：回覆第一個非空白字元是數字（balance 行）
            const char *p = recvbuf;
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
            int looks_like_list = (*p >= '0' && *p <= '9');

            if ( (strncasecmp(line, "List", 4) == 0 ||
                (strchr(line, '#') && strncasecmp(line, "REGISTER#", 9) != 0))
                && looks_like_list ) {
                update_online_users(recvbuf);
            }
        }

        // ------------------------------------------------------------------
        // 🔵 D. 偵測是否有人連進來（P2P 收款）
        // ------------------------------------------------------------------
        if (listen_sd != -1) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_sd, &rfds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 0;

            int ready = select(listen_sd + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET(listen_sd, &rfds)) {
                handle_incoming_p2p(listen_sd, sd);
            }
        }

        // ------------------------------------------------------------------
        // 🔴 E. Exit 指令 → 結束程式
        // ------------------------------------------------------------------
        if (strcasecmp(line, "Exit") == 0) break;
    }

    // ------------------------------------------------------------------
    // 🏁 F. 結束連線並關閉所有 socket
    // ------------------------------------------------------------------
    close(sd);
    if (listen_sd != -1) close(listen_sd);
    printf("[INFO] Connection closed.\n");
    return 0;
}












