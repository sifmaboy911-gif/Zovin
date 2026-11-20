#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <dirent.h>

// Оффсеты (те же, что были в Java)
const uintptr_t OFF_CLIENT_WORLD = 0xCD2F9B0;
const uintptr_t OFF_REGISTRY = 0x40;
const uintptr_t OFF_WORLD = 0x20;
const uintptr_t OFF_STORE = 0x30;
const uintptr_t OFF_OBJECTS = 0x18;
const uintptr_t OFF_LIST_ITEMS = 0x10;
const uintptr_t OFF_LIST_COUNT = 0x18;
const uintptr_t OFF_PLAYER_TRANSFORM = 0x20;
const uintptr_t OFF_TRANSFORM_POS = 0x38;
const uintptr_t OFF_PLAYER_STATS = 0x70;
const uintptr_t OFF_STATS_HP = 0x10;
const uintptr_t OFF_VIEW_MATRIX = 0xCD32028;

int gamePid = -1;
uintptr_t baseAddr = 0;
float viewMatrix[16];

// Поиск PID игры
int getPid(const char* packageName) {
    std::string cmd = "pidof " + std::string(packageName);
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    int pid = -1;
    if (fscanf(fp, "%d", &pid) != 1) pid = -1;
    pclose(fp);
    return pid;
}

// Поиск базы libil2cpp.so
uintptr_t getModuleBase(int pid, const char* moduleName) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(moduleName) != std::string::npos && line.find("r-xp") != std::string::npos) {
            size_t pos = line.find('-');
            if (pos != std::string::npos) {
                return std::stoull(line.substr(0, pos), nullptr, 16);
            }
        }
    }
    return 0;
}

// Чтение памяти через process_vm_readv
bool readMem(int pid, uintptr_t address, void* buffer, size_t size) {
    struct iovec local[1];
    struct iovec remote[1];
    local[0].iov_base = buffer;
    local[0].iov_len = size;
    remote[0].iov_base = (void*)address;
    remote[0].iov_len = size;
    ssize_t nread = process_vm_readv(pid, local, 1, remote, 1, 0);
    return nread == size;
}

template <typename T>
T read(int pid, uintptr_t address) {
    T value = 0;
    readMem(pid, address, &value, sizeof(T));
    return value;
}

// Перевод 3D в 2D
bool worldToScreen(float x, float y, float z, float* outX, float* outY) {
    float w = viewMatrix[3] * x + viewMatrix[7] * y + viewMatrix[11] * z + viewMatrix[15];
    if (w < 0.1f) return false;
    float screenX = (viewMatrix[0] * x + viewMatrix[4] * y + viewMatrix[8] * z + viewMatrix[12]) / w;
    float screenY = (viewMatrix[1] * x + viewMatrix[5] * y + viewMatrix[9] * z + viewMatrix[13]) / w;
    
    // Экран телефона (примерно 1080x2400)
    *outX = (screenX + 1.0f) / 2.0f * 1080.0f;
    *outY = (1.0f - screenY) / 2.0f * 2400.0f;
    return true;
}

int main() {
    // 1. Создаем TCP сервер для связи с Java
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8888);
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);
    
    std::cout << "ESP Server started on port 8888" << std::endl;

    while (true) {
        // Ждем подключения от Java
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) continue;

        // Ищем игру
        gamePid = getPid("com.axlebolt.standoff2");
        if (gamePid != -1) {
            baseAddr = getModuleBase(gamePid, "libil2cpp.so");
        }

        if (gamePid == -1 || baseAddr == 0) {
            std::string err = "ERR|Game not found";
            send(client_socket, err.c_str(), err.length(), 0);
            close(client_socket);
            sleep(1);
            continue;
        }

        // Читаем матрицу
        readMem(gamePid, baseAddr + OFF_VIEW_MATRIX, viewMatrix, sizeof(viewMatrix));

        uintptr_t clientWorld = read<uintptr_t>(gamePid, baseAddr + OFF_CLIENT_WORLD);
        if (clientWorld == 0) {
            std::string err = "ERR|clientWorld = 0";
            send(client_socket, err.c_str(), err.length(), 0);
            close(client_socket);
            sleep(1);
            continue;
        }

        uintptr_t registry = read<uintptr_t>(gamePid, clientWorld + OFF_REGISTRY);
        uintptr_t world = read<uintptr_t>(gamePid, registry + OFF_WORLD);
        uintptr_t store = read<uintptr_t>(gamePid, world + OFF_STORE);
        uintptr_t objectsList = read<uintptr_t>(gamePid, store + OFF_OBJECTS);
        
        if (objectsList == 0) {
            close(client_socket);
            sleep(1);
            continue;
        }

        uintptr_t itemsArray = read<uintptr_t>(gamePid, objectsList + OFF_LIST_ITEMS);
        int count = read<int>(gamePid, objectsList + OFF_LIST_COUNT);

        std::string response = "OK|";
        
        if (itemsArray != 0 && count > 0 && count <= 100) {
            for (int i = 0; i < count; i++) {
                uintptr_t playerObj = read<uintptr_t>(gamePid, itemsArray + (i * 8));
                if (playerObj == 0) continue;

                uintptr_t statsObj = read<uintptr_t>(gamePid, playerObj + OFF_PLAYER_STATS);
                if (statsObj != 0) {
                    int hp = read<int>(gamePid, statsObj + OFF_STATS_HP);
                    if (hp <= 0) continue;
                }

                uintptr_t transform = read<uintptr_t>(gamePid, playerObj + OFF_PLAYER_TRANSFORM);
                if (transform == 0) continue;

                float wx = read<float>(gamePid, transform + OFF_TRANSFORM_POS);
                float wy = read<float>(gamePid, transform + OFF_TRANSFORM_POS + 4);
                float wz = read<float>(gamePid, transform + OFF_TRANSFORM_POS + 8);

                float sx, sy;
                if (worldToScreen(wx, wy, wz, &sx, &sy)) {
                    response += std::to_string(sx) + "," + std::to_string(sy) + ";";
                }
            }
        }

        // Отправляем координаты в Java
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);

        // Задержка (около 30 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    return 0;
}
