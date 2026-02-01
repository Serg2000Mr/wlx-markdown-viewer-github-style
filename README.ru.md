# Markdown Lister Plugin для Total Commander (64-битная версия)

Основано на [плагине wlx-markdown-viewer](https://github.com/rg-software/wlx-markdown-viewer), 
обновлено для использования процессора [Markdig](https://github.com/xoofx/markdig) с поддержкой современного синтаксиса Markdown.

**Ключевое улучшение:** Эта версия использует .NET Native AOT для компиляции движка Markdig в автономную нативную DLL. 
**Конечному пользователю НЕ требуется установка .NET Runtime.**

## Возможности
- Полная поддержка современного Markdown (GFM, таблицы, эмодзи и т.д.) через Markdig.
- Автономность: Отсутствие внешних зависимостей от .NET Runtime.
- Современный дизайн в стиле GitHub.
- Быстрая и легкая работа.

## Тонкая настройка

Конфигурация плагина задается в файле `MarkdownView.ini`. Настройки Markdown:

- `Extensions: MarkdownExtensions` — расширения файлов, распознаваемые плагином как Markdown.
- `Renderer: Extensions` — коллекция расширений для процессора Markdig. [Подробнее о расширениях Markdig](https://github.com/xoofx/markdig/blob/master/readme.md)  
  Поддерживаются следующие расширения: common, advanced, alerts, pipetables, gfm-pipetables, emphasisextras, listextras, hardlinebreak, footnotes, footers, citations, attributes, gridtables, abbreviations, emojis, definitionlists, customcontainers, figures, mathematics, bootstrap, medialinks, smartypants, autoidentifiers, tasklists, diagrams, nofollowlinks, noopenerlinks, noreferrerlinks, nohtml, yaml, nonascii-noescape, autolinks, globalization.
- `Renderer: CustomCSS` — путь к файлу CSS для настройки внешнего вида документа. В комплект включены стили от [Markdown CSS](https://markdowncss.github.io/) и темы в стиле Github от S. Kuznetsov.

## Обновление Internet Explorer

Плагин использует движок Internet Explorer, который можно обновить через [реестр](https://github.com/rg-software/wlx-markdown-viewer/raw/master/ie_upgrade_registry.zip) (подробности в [MSDN](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/general-info/ee330730(v=vs.85)?redirectedfrom=MSDN#browser-emulation)).

## Установка

Архив с бинарными файлами плагина поставляется со скриптом установки. Просто откройте архив в Total Commander и подтвердите установку.
