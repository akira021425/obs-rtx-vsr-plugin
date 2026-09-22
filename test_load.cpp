#include <windows.h>
#include <iostream>

int main() {
    HMODULE mod = LoadLibraryA("C:\\Program Files\\obs-studio\\bin\\64bit\\NvOFFRUC.dll");
    if (!mod) {
        std::cout << "Failed to load, error code: " << GetLastError() << std::endl;
    } else {
        std::cout << "Successfully loaded." << std::endl;
    }
    return 0;
}
