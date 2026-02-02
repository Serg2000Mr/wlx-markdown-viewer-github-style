# CSS Improvements for GitHub Styling

## Изменения / Changes

Обновлены CSS файлы для более точного соответствия стилям GitHub (2025):

### Файлы / Files Modified
- `Build/css/github.css` - светлая тема / light theme
- `Build/css/github.dark.css` - темная тема / dark theme

### Основные изменения / Main Changes

#### 1. Цветовая палитра / Color Palette
- **Текст body**: `#24292f` → `#1f2328` (light), `#c9d1d9` → `#e6edf3` (dark)
- **Inline code фон**: `#D8DEE4` → `rgba(175,184,193,0.2)` (light), `#161b22` → `rgba(110,118,129,0.4)` (dark)
- **Границы**: `#d0d7de` → `#d1d9e0` (light), `#30363d` → `#3d444d` (dark)
- **Ссылки**: `#58a6ff` → `#4493f8` (dark)

#### 2. Заголовки / Headers
- **H1**: `2.15em` → `2em`
- **H2**: `1.75em` → `1.5em`
- **H3**: `1.5em` → `1.25em`
- **H4**: `1.25em` → `1em`
- **H5**: `1.1em` → `0.875em`
- **H6**: `1em` → `0.85em`
- Унифицированы отступы: `margin-top: 24px`, `margin-bottom: 16px`

#### 3. Код / Code
- Убраны псевдоэлементы `::before` и `::after` с пробелами
- Обновлен font-family: `ui-monospace,SFMono-Regular,SF Mono,Menlo,Consolas,Liberation Mono,monospace`
- Размер шрифта: `90%` → `85%`
- Padding для inline code: `0.15em 0.1em` → `0.2em 0.4em`
- Border-radius: `3px` → `6px`

#### 4. Блоки кода / Code Blocks
- Убрана граница (border) в светлой теме
- Padding: `1.2em` → `16px`
- Line-height: `1.4` → `1.45`

#### 5. Цитаты / Blockquotes
- Убран фон в светлой теме (`background-color: #EDEFF3` → transparent)
- Border-left: `4px` → `0.25em`
- Padding: `5px 15px` → `0 1em`

#### 6. Горизонтальные линии / Horizontal Rules
- Height: `3px` → `0.25em`
- Margin: `32px 0` → `24px 0`

#### 7. Таблицы / Tables
- Убран фон заголовков в светлой теме
- Чередующиеся строки: фон `#f6f8fa` (light), `#161b22` (dark)
- Упрощены стили границ

#### 8. Отступы body / Body Padding
- Padding: `15px` → `32px`
- Убран `margin-left: 1em`

#### 9. Иконки якорей заголовков / Header Anchor Icons
- Исправлено позиционирование для всех уровней заголовков
- Использован `display: flex` с `align-items: center` для универсального выравнивания
- Высота контейнера: `height: 1.25em` (адаптируется к размеру шрифта)
- Позиция: `top: 0` вместо `top: 50%` с transform
- Обновлен цвет иконки в темной теме: `#c9d1d9` → `#e6edf3`

#### 10. Поддержка диаграмм Mermaid.js / Mermaid.js Diagrams Support
- Добавлено расширение `diagrams` в Markdig pipeline
- Подключена библиотека Mermaid.js через CDN (v10)
- Включено выполнение JavaScript: `AllowScripting=1` в конфигурации
- Настроена автоматическая инициализация диаграмм при загрузке страницы

## Тестирование / Testing

Для проверки изменений:

1. Скомпилируйте проект: `BuildAll.bat`
2. Откройте тестовый файл в Total Commander: `Примеры\Вся разметка\format-README\README.md`
3. Сравните с GitHub версией: `Примеры\Вся разметка GitHub\GnuriaN_format-README_ Формат файла README.html`

## Совместимость / Compatibility

- Изменения обратно совместимы
- Все существующие markdown файлы будут отображаться корректно
- Поддерживаются все расширения Markdig

## Дальнейшие улучшения / Future Improvements

- Добавить поддержку GitHub Alerts (уже есть базовые стили)
- Добавить больше тем (dimmed, retro и т.д.)
- Рассмотреть возможность локального кэширования Mermaid.js

## Измененные файлы / Modified Files

- `Build/css/github.css` - обновлены стили светлой темы
- `Build/css/github.dark.css` - обновлены стили темной темы
- `MarkdigNative/Lib.cs` - добавлена поддержка Mermaid.js диаграмм
- `Build/MarkdownView.ini` - включены diagrams и JavaScript
- `CSS_IMPROVEMENTS.md` - документация изменений (создан)
- `QUICK_START.ru.md` - инструкция по использованию (создан)
