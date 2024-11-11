#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

// 定義最大參數數量
#define MAX_ARGS 64

// 全局變數存儲動態命令列表
const char **dynamic_commands = NULL;
size_t dynamic_commands_count = 0;

// 自定義補全函數的前向宣告
char **custom_completion(const char *text, int start, int end);
char *command_generator(const char *text, int state);

// 函數來動態獲取命令列表
void load_dynamic_commands() {
    char *path_env = getenv("PATH");
    if (!path_env) return;

    char *path = strdup(path_env);
    if (!path) {
        perror("strdup failed");
        exit(EXIT_FAILURE);
    }

    char *dir = strtok(path, ":");
    while (dir != NULL) {
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                // 濾除 "." 和 ".."
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;

                // 檢查是否為可執行文件
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
                struct stat sb;
                if (stat(full_path, &sb) == 0 && (sb.st_mode & S_IXUSR)) {
                    // 添加到命令列表
                    dynamic_commands = realloc(dynamic_commands, sizeof(char*) * (dynamic_commands_count + 2));
                    if (!dynamic_commands) {
                        perror("realloc failed");
                        free(path);
                        exit(EXIT_FAILURE);
                    }
                    dynamic_commands[dynamic_commands_count++] = strdup(entry->d_name);
                    dynamic_commands[dynamic_commands_count] = NULL; // 結尾設為 NULL
                }
            }
            closedir(dp);
        }
        dir = strtok(NULL, ":");
    }
    free(path);
}

// Signal handler for SIGINT
void sigint_handler(int sig) {
    // 在接收到 SIGINT 時，清除當前輸入並顯示新的提示符
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

int main() {
    char *input;
    char *args[MAX_ARGS];
    char *token;
    pid_t pid;
    int status;

    // 加載動態命令
    load_dynamic_commands();

    // 安裝 SIGINT 處理程序
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // 重新啟動被中斷的系統調用
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    // 初始化 Readline 補全
    rl_attempted_completion_function = custom_completion;

    while (1) {
        // 取得使用者名稱
        struct passwd *pw = getpwuid(getuid());
        if (!pw) {
            perror("getpwuid failed");
            exit(EXIT_FAILURE);
        }
        const char *username = pw->pw_name;

        // 取得目前工作目錄
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd error");
            continue;
        }

        // 构建提示符
        char prompt[PATH_MAX + 64];
        snprintf(prompt, sizeof(prompt), "%s:%s$ ", username, cwd);

        // 使用 readline 讀取輸入
        input = readline(prompt);

        // 判斷 EOF（如按下 Ctrl+D）
        if (input == NULL) {
            printf("\n");
            break;
        }

        // 去除輸入前後的空白（Readline 已處理大部分情況，這裡僅示範）
        char *trimmed_input = input;

        // 判斷是否輸入 exit
        if (trimmed_input && strcmp(trimmed_input, "exit") == 0) {
            free(input);
            break;
        }

        // 如果輸入不為空，加入歷史記錄
        if (trimmed_input && *trimmed_input) {
            add_history(trimmed_input);
        }

        // 解析輸入
        int arg_count = 0;
        token = strtok(trimmed_input, " ");
        while (token != NULL && arg_count < MAX_ARGS - 1) {
            args[arg_count++] = token;
            token = strtok(NULL, " ");
        }
        args[arg_count] = NULL;

        // 空輸入處理
        if (arg_count == 0) {
            free(input);
            continue;
        }

        // 檢查是否為內建指令 'cd'
        if (strcmp(args[0], "cd") == 0) {
            // 處理 'cd' 指令
            if (arg_count < 2) {
                // 如果沒有參數，跳轉到用戶主目錄
                const char *home_dir = getenv("HOME");
                if (home_dir == NULL) {
                    home_dir = pw->pw_dir;
                }
                if (chdir(home_dir) != 0) {
                    perror("cd");
                }
            } else {
                // 跳轉到指定目錄
                if (chdir(args[1]) != 0) {
                    perror("cd");
                }
            }
            free(input);
            continue;
        }

        // 創建子程序
        pid = fork();
        if (pid < 0) {
            perror("fork error");
            free(input);
            continue;
        } else if (pid == 0) {
            // 子程序，恢復 SIGINT 的預設行為
            struct sigaction sa_child;
            sa_child.sa_handler = SIG_DFL;
            sigemptyset(&sa_child.sa_mask);
            sa_child.sa_flags = 0;
            if (sigaction(SIGINT, &sa_child, NULL) == -1) {
                perror("sigaction in child");
                exit(EXIT_FAILURE);
            }

            // 執行命令
            execvp(args[0], args);
            // 如果 execvp 返回，說明執行失敗
            perror("Command execution failed");
            exit(EXIT_FAILURE);
        } else {
            // 父程序，等待子程序結束
            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid error");
            }
        }

        free(input);
    }

    // 釋放動態命令列表的記憶體
    for (size_t i = 0; i < dynamic_commands_count; i++) {
        free((void *)dynamic_commands[i]);
    }
    free((void *)dynamic_commands);

    return 0;
}

// 自定義補全函數
char **custom_completion(const char *text, int start, int end) {
    // 當前補全在命令的第一個參數時，補全命令
    // 否則，使用默認的文件補全
    if (start == 0) {
        return rl_completion_matches(text, command_generator);
    } else {
        // 使用默認的文件補全
        return rl_completion_matches(text, rl_filename_completion_function);
    }
}

// 命令生成器，用於生成命令補全
char *command_generator(const char *text, int state) {
    static size_t list_index;
    const char *cmd;

    if (state == 0) {
        list_index = 0;
    }

    while (list_index < dynamic_commands_count) {
        cmd = dynamic_commands[list_index++];
        if (strncmp(cmd, text, strlen(text)) == 0) {
            return strdup(cmd);
        }
    }

    return NULL;
}
