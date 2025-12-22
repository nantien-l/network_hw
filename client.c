// client.c HW1 Part1 minimal client
// 編譯: gcc -Wall -Wextra -O2 client.c -o client
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
#include <asm-generic/socket.h>

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
int my_balance = 10000;                 // 本地紀錄的餘額，預設 10000

// 0 = IDLE (閒置)
// 1 = AWAITING_TRANSFER_OK (剛P2P收款，已送指令給Server，等待 "Transfer OK")
// 2 = AWAITING_LIST_AFTER_TRANSFER (剛收到 "Transfer OK"，已送 "List"，等待列表)
int g_client_state = 0;


static void rstrip(char *s);                                                   //移除字串 s 尾端的換行或空白字元（就地修改），避免末尾殘留 '\n'、'\r' 或空格。        
static int connect_tcp(const char* ip, int port);                              //建立一個 TCP 連線到指定 ip:port，成功回傳 socket fd，失敗回傳 -1。
static int connect_peer(const char* ip, int port);                             //建立到 peer（另一個 client）的 TCP 連線，通常用於 P2P 傳輸；成功回傳 socket fd，失敗回傳 -1。  
static int create_listen_socket(int port);                                     //建立、綁定並 listen 在指定 port 的 TCP listening socket，回傳 listening socket fd，失敗回傳 -1。
static OnlineUser* find_online_user(const char* username);                     //在本地維護的 OnlineUser 清單中搜尋 username，若找到回傳指標，否則回傳 NULL。
static void send_line(int sd, const char* msg);                                //將 msg 透過非 TLS 的 socket sd 傳送出去，確保整行送出。
static void send_line_ssl(SSL *ssl, const char* msg);                          //將 msg 透過 SSL/TLS 連線 ssl 傳送出去，確保整行送出。
static int recv_full_burst(int sd, char *out, int out_sz);                     //從 socket sd 讀取可用的資料塊直到沒有資料或緩衝滿為止，將資料寫入 out，回傳讀到的位元組數。
static void update_online_users(const char* response);                         //解析伺服器回傳的線上使用者列表 response，更新本地的 OnlineUser 清單（新增/移除/更新狀態）。
static void handle_p2p_transfer(const char* receiver, int amount);       //發起對 receiver 的 P2P 轉帳流程：可能先通知伺服器取得對方資訊，建立 P2P 連線並傳送金額等資料。
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
    setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));  // 新增這行！

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

    printf("\n[INFO] Listening on port %d for P2P transfers\n", port);
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
// 透過 SSL/TLS 送資料
//------------------------------------------------------------------------------
static void send_line_ssl(SSL *ssl, const char* msg) {
    char buf[BUFSZ];
    int len = snprintf(buf, sizeof(buf), "%s", msg);

    int n = SSL_write(ssl, buf, len);
    if (n <= 0) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }
}




//------------------------------------------------------------------------------
// 從 server 收一整「波」資料（避免 Transfer OK 黏到下一條）
//------------------------------------------------------------------------------
static int recv_full_burst(int sd, char *out, int out_sz) {
    int total = 0;
    out[0] = '\0';

    // 第一次阻塞收資料
    int n = recv(sd, out, out_sz - 1, 0);
    if (n <= 0) return n;
    total += n;
    out[total] = '\0';

    // 接下來稍微等一下，看有沒有後續資料
    fd_set rfds;
    struct timeval tv;
    while (total < out_sz - 1) {
        FD_ZERO(&rfds);
        FD_SET(sd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;  // 最多再等 200ms
        int ready = select(sd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) break;    // 超時或錯誤
        if (!FD_ISSET(sd, &rfds)) break;

        n = recv(sd, out + total, out_sz - 1 - total, MSG_DONTWAIT);
        if (n <= 0) break;
        total += n;
        out[total] = '\0';
    }
    return total;
}



static int recv_full_burst_ssl(SSL *ssl, char *out, int out_sz) {
    int total = 0;
    out[0] = '\0';

    int n = SSL_read(ssl, out, out_sz - 1);
    if (n <= 0) return n;

    total += n;
    out[total] = '\0';
    return total;
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
            char *endptr = NULL;
            long parsed = strtol(line, &endptr, 10);
            if (endptr != line) {
                if (parsed < 0) parsed = 0;
                my_balance = (int)parsed;
            }
            printf("Balance: %d\n", my_balance);
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

    printf("[INFO] %d users online:\n", num_online_users);
    for (int i = 0; i < num_online_users; i++) {
        printf("  - %s@%s:%d\n",
               online_users[i].username,
               online_users[i].ip,
               online_users[i].port);
    }
}

//------------------------------------------------------------------------------
// P2P 轉帳：只負責通知對方，不碰 server socket
//------------------------------------------------------------------------------
static void handle_p2p_transfer(const char* receiver, int amount) {

    if (my_name[0] == '\0') {
        printf("[WARN] 尚未登入，無法轉帳。\n");
        return;
    }
    if (amount <= 0) {
        printf("[WARN] 轉帳金額需為正整數。\n");
        return;
    }
    if (amount > my_balance) {
        printf("[WARN] 餘額不足。\n");
        return;
    }

    OnlineUser* target = find_online_user(receiver);
    if (!target) {
        printf("[WARN] Receiver '%s' 未在線上。\n", receiver);
        return;
    }

    // 建立 P2P 連線
    int peer_sd = connect_peer(target->ip, target->port);
    if (peer_sd < 0) {
        printf("[WARN] 無法連線至 %s@%s:%d\n", receiver, target->ip, target->port);
        return;
    }

    // 傳送訊息
    char msg[BUFSZ];
    snprintf(msg, sizeof(msg), "%s#%d#%s", my_name, amount, receiver);

    ssize_t n = send(peer_sd, msg, strlen(msg), 0);
    if (n < 0) {
        perror("[P2P] send");
        close(peer_sd);
        return;
    }

    shutdown(peer_sd, SHUT_WR);
    close(peer_sd);

    printf("[INFO] 已送出轉帳請求：%s → %s (%d)\n",
           my_name, receiver, amount);

    // ❗❗ 不要 List、不睡、不 drain server
    // 由於對方收到後會 pending_cmd → main 下一輪會自動更新
}



//------------------------------------------------------------------------------
// 處理來自其他 client 的 P2P 傳入連線（完全無阻塞版）
//------------------------------------------------------------------------------
static void handle_incoming_p2p(int p2p_sd, int server_sd) {

    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int conn = accept(p2p_sd, (struct sockaddr*)&cli, &len);
    if (conn < 0) { perror("accept"); return; }

    char buf[BUFSZ];
    int n = recv(conn, buf, sizeof(buf)-1, 0);
    if (n <= 0) { close(conn); return; }

    buf[n] = '\0';

    char sender[64], receiver[64];
    int amount = 0;

    if (sscanf(buf, "%[^#]#%d#%s", sender, &amount, receiver) != 3) {
        printf("[P2P] Invalid transfer format: %s\n", buf);
        close(conn);
        return;
    }

    // 1. 立刻印出訊息並強制刷新 (fflush)，確保使用者一定看得到
    printf("\n[P2P] %s sent you %d\n", sender, amount);
    fflush(stdout); 

    // 2. 直接將指令轉發給 Server
    send_line(server_sd, buf); 
    
    // 3. 更新全域狀態
    g_client_state = 1; // 進入 "AWAITING_TRANSFER_OK" 狀態

    close(conn);
}



//------------------------------------------------------------------------------
// 顯示目前使用者資訊 + 指令方框
//------------------------------------------------------------------------------
static void show_menu() {
    const char *name;
    const char *ip = "N/A";
    int port_to_show = my_port;

    // 如果還沒登入，就顯示 (not login)
    if (my_name[0] == '\0') {
        name = "(not login)";
    } else {
        name = my_name;
        // 試著從 online_users 找自己的 IP / port
        OnlineUser *me = find_online_user(my_name);
        if (me) {
            ip = me->ip;
            port_to_show = me->port;
        }
    }

    printf("\n#==================== User Info ====================#\n\n");
    printf("User : %-12s  Balance: %d\n", name, my_balance);
    printf("IP   : %-12s  Port   : %d\n", ip, port_to_show);
    printf("\n");
    printf(
        "┌──────────────────────── Commands ──────────────────────────┐\n"
        "│  REGISTER#name          a#amount#b             Exit        │\n"
        "│  name#port              List                               │\n"
        "└────────────────────────────────────────────────────────────┘\n"
        "> "
    );
    fflush(stdout);   // 確保馬上印出來
}



//------------------------------------------------------------------------------
// 主程式：互動式 loop
// 使用方式：./client <server_ip> <server_port>
//------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // 初始化 OpenSSL 函式庫
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }


    int sd = connect_tcp(ip, port);

    // 建立 SSL 連線
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sd);

    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    printf("\n===== Connected to %s:%d =====\n", ip, port);
    show_menu();

    char line[BUFSZ];
    char recvbuf[BUFSZ];

    while (1) {

        // ================================
        // 💥 使用 select 同時監聽：
        //   1. 標準輸入（keyboard）
        //   2. P2P 連線
        //   3. server socket
        // ================================
        fd_set rfds;                    // 讀取事件集合
        FD_ZERO(&rfds);                 // 清空集合

        FD_SET(STDIN_FILENO, &rfds);            // 標準輸入
        FD_SET(sd, &rfds);                       // 伺服器 socket
        if (listen_sd != -1) FD_SET(listen_sd, &rfds); // P2P 監聽 socket       

        int maxfd = sd;
        if (listen_sd > maxfd) maxfd = listen_sd;

        // 每次等 0.5 秒，不阻塞
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            perror("select");
            continue;
        }

        // ================================
        // 🔵 1. 處理 P2P 收款事件
        // ================================
        if (listen_sd != -1 && FD_ISSET(listen_sd, &rfds)) {
            handle_incoming_p2p(listen_sd, sd);

            if (g_client_state == 1) {
                // 狀態 1：我們剛收到了 "Transfer OK" (推測)
                // 接著，我們必須送出 "List" 來更新餘額
                send_line(sd, "List");
                g_client_state = 2; // 進入 "AWAITING_LIST_AFTER_TRANSFER" 狀態
            } 
        }

        

        // ================================
        // 🟣 3. 處理 server 主動回覆（例如錯誤）
        // ================================
        if (FD_ISSET(sd, &rfds)) {
            memset(recvbuf, 0, sizeof(recvbuf));
            int n = SSL_read(ssl, recvbuf, sizeof(recvbuf) - 1);
            if (n > 0) recvbuf[n] = '\0'; // recv() 不會自動加結尾，手動加上

            
            if (n <= 0) {
                printf("[INFO] Server closed.\n");
                break;
            }
            
            // 無論如何，都先印出收到的訊息
            printf("\n\n#====== Server Reply: ======#\n");
            printf("%s", recvbuf);
            printf("#===========================#\n\n");


            // 根據我們的狀態機，決定下一步動作
            if (g_client_state == 1) {
                // 狀態 1：我們剛收到了 "Transfer OK" (推測)
                // 接著，我們必須送出 "List" 來更新餘額
                send_line(sd, "List");
                g_client_state = 2; // 進入 "AWAITING_LIST_AFTER_TRANSFER" 狀態
            } 
            else if (g_client_state == 2) {
                // 狀態 2：我們剛收到了 "List" 的回覆
                // 更新餘額和列表
                update_online_users(recvbuf);
                g_client_state = 0; // 回到 IDLE 閒置狀態
            }
            // 如果 g_client_state == 0，代表這只是 Server 的一般訊息
            // (例如別人登入登出)，我們印出訊息就好，不用做任何事。
        }

        // ================================
        // 🟢 4. 處理使用者輸入
        // ================================
        if (FD_ISSET(STDIN_FILENO, &rfds)) {

            if (!fgets(line, sizeof(line), stdin)) break;
            rstrip(line);
            if (!*line) continue;

            // -------------------------------
            // 🔵 判斷是否轉帳
            // -------------------------------
            char sender[64], receiver[64];
            int amount;

            if (sscanf(line, "%[^#]#%d#%s", sender, &amount, receiver) == 3) {

                if (my_name[0] == '\0') {
                    printf("[WARN] 尚未登入\n");
                    continue;
                }
                if (strcasecmp(sender, my_name) != 0) {
                    printf("[WARN] 只能用自己名字轉帳\n");
                    continue;
                }
                if (strcmp(receiver, my_name) == 0) {
                    printf("[WARN] 不能轉給自己\n");
                    continue;
                }

                printf("\n[LOCAL] 確認 %s → %s (%d)\n", sender, receiver, amount);
                handle_p2p_transfer(receiver, amount);
                continue;
            }

            // -------------------------------
            // 🟢 若是 name#port → 登入
            // -------------------------------
            if (strchr(line, '#') && strncasecmp(line, "REGISTER#", 9) != 0) {

                char *sharp = strchr(line, '#');
                int portnum = atoi(sharp + 1);

                strncpy(my_name, line, sharp - line);
                my_name[sharp - line] = '\0';
                my_port = portnum;

                listen_sd = create_listen_socket(portnum);
            }

            // -------------------------------
            // 🟣 一般指令送 server
            // -------------------------------

            if (g_client_state != 0) {
                printf("[WARN] 正在處理 P2P 轉帳，請稍候...\n");
                continue;
            }

            send_line_ssl(ssl, line);

            memset(recvbuf, 0, sizeof(recvbuf));
            int n = recv_full_burst_ssl(ssl, recvbuf, sizeof(recvbuf));
            if (n > 0) {
                printf("\n\n#====== Server Reply: ======#\n");
                printf("%s", recvbuf);

                // [修正] 只有當指令不是 REGISTER 且不是 Exit 時，才嘗試更新列表
                // 這樣就不會把 "100 OK" 當成餘額 100 元了
                if (strncasecmp(line, "REGISTER", 8) != 0 && strcasecmp(line, "Exit") != 0) {
                    update_online_users(recvbuf);
                }
                
                printf("#===========================#\n\n");
            }

            if (strcasecmp(line, "Exit") == 0)
                break;
            
            show_menu();
        }

    }

    close(sd);
    if (listen_sd != -1) close(listen_sd);
    return 0;
}

