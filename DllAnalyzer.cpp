#include <windows.h>
#include <iostream>
#include <string>

int main() {
    std::cout << "=== Анализ зависимостей DLL ===" << std::endl;

    // Тест загрузки Markdown-x64.dll
    std::cout << "\n1. Тестирование Markdown-x64.dll:" << std::endl;
    HMODULE hMarkdown = LoadLibraryA("Markdown-x64.dll");
    if (hMarkdown) {
        std::cout << "[OK] Markdown-x64.dll загружена успешно" << std::endl;
        FreeLibrary(hMarkdown);
    } else {
        DWORD error = GetLastError();
        std::cout << "[FAIL] Ошибка загрузки Markdown-x64.dll. Код: " << error << std::endl;

        // Расшифровка основных ошибок
        switch (error) {
            case 126: std::cout << "Модуль не найден" << std::endl; break;
            case 127: std::cout << "Процедура не найдена" << std::endl; break;
            case 193: std::cout << "Неверный образ Win32" << std::endl; break;
            default: std::cout << "Неизвестная ошибка" << std::endl; break;
        }
    }

    // Тест загрузки MarkdigNative-x64.dll
    std::cout << "\n2. Тестирование MarkdigNative-x64.dll:" << std::endl;
    HMODULE hMarkdig = LoadLibraryA("MarkdigNative-x64.dll");
    if (hMarkdig) {
        std::cout << "[OK] MarkdigNative-x64.dll загружена успешно" << std::endl;
        FreeLibrary(hMarkdig);
    } else {
        DWORD error = GetLastError();
        std::cout << "[FAIL] Ошибка загрузки MarkdigNative-x64.dll. Код: " << error << std::endl;
    }

    // Информация о системе
    std::cout << "\n3. Информация о системе:" << std::endl;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::cout << "Архитектура процессора: " << si.wProcessorArchitecture << std::endl;

    #ifdef _WIN64
        std::cout << "Приложение: 64-bit" << std::endl;
    #else
        std::cout << "Приложение: 32-bit" << std::endl;
    #endif

    system("pause");
    return 0;
}