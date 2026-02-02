# Быстрый старт / Quick Start

## Что было сделано

Обновлены CSS стили для более точного соответствия GitHub (2025):
- Исправлены размеры заголовков
- Обновлена цветовая палитра
- Улучшено отображение кода
- Упрощены стили цитат и таблиц

## Как использовать

### 1. Сборка проекта

```cmd
BuildAll.bat
```

**Примечание**: Для установки в `C:\Program Files\totalcmd\plugins\wlx\` требуются права администратора.

### 2. Установка вручную

Если BuildAll.bat не может установить плагин автоматически:

1. Создайте папку (если её нет):
   ```
   C:\Program Files\totalcmd\plugins\wlx\MarkdownView
   ```

2. Скопируйте файлы из `bin\Release\`:
   - `MarkdownView.wlx64`
   - `Markdown-x64.dll`
   - `MarkdigNative-x64.dll`
   - `MarkdownView.ini`
   - Папку `css\` целиком

3. В Total Commander:
   - Конфигурация → Настройка → Плагины → Lister
   - Добавить → Указать путь к `MarkdownView.wlx64`

### 3. Проверка результата

Откройте в Total Commander:
```
Примеры\Вся разметка\format-README\README.md
```

Сравните с GitHub версией:
```
Примеры\Вся разметка GitHub\GnuriaN_format-README_ Формат файла README.html
```

## Настройка тем

В файле `MarkdownView.ini` (секция `[Renderer]`):

```ini
[Renderer]
Extensions=common+advanced+emojis+mathematics+tasklists
CustomCSS=css\github.css          ; Светлая тема
CustomCSSDark=css\github.dark.css ; Темная тема
```

Доступные темы:
- `github.css` - светлая (обновлена)
- `github.dark.css` - темная (обновлена)
- `github.dimmed.css` - приглушенная
- `github.retro.css` - ретро светлая
- `github.retro.dark.css` - ретро темная
- `github.retro.dimmed.css` - ретро приглушенная
- `air.css`, `modest.css`, `retro.css`, `splendor.css` - альтернативные

## Поддержка Markdown

Плагин поддерживает:
- GitHub Flavored Markdown (GFM)
- Таблицы
- Списки задач
- Эмодзи
- Математические формулы
- **Диаграммы Mermaid.js** (новое!)
- Footnotes
- И многое другое

### Пример диаграммы Mermaid:

\`\`\`mermaid
graph TD
    A[Start] --> B{Decision}
    B -->|Yes| C[OK]
    B -->|No| D[Cancel]
\`\`\`

Подробнее: [Readme.md](Readme.md)

## Проблемы?

1. **Плагин не устанавливается**: Запустите `BuildAll.bat` от имени администратора
2. **Стили не применяются**: Проверьте, что папка `css\` скопирована рядом с `.wlx64` файлом
3. **Диаграммы Mermaid не отображаются**: Убедитесь, что `AllowScripting=1` в `MarkdownView.ini` и есть доступ к интернету для загрузки библиотеки
4. **Ошибки компиляции**: Убедитесь, что установлены Visual Studio 2026 и .NET 8 SDK

## Дополнительная информация

- [CSS_IMPROVEMENTS.md](CSS_IMPROVEMENTS.md) - детали изменений CSS
- [Readme.md](Readme.md) - полная документация
- [AGENTS.ru.md](AGENTS.ru.md) - информация для разработчиков
