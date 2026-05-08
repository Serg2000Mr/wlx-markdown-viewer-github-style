# Проверка масштаба Mermaid

Этот файл нужен для ручной проверки: при `Ctrl+колесо` должен увеличиваться не только обычный текст, но и Mermaid-диаграммы ниже.

## Простая диаграмма

```mermaid
flowchart TD
    A[Открыть Markdown] --> B[Markdig собирает HTML]
    B --> C[WebView2 показывает документ]
    C --> D[Mermaid рисует SVG]
```

## Широкая диаграмма

```mermaid
flowchart LR
    A[Markdown-файл] --> B[Чтение файла]
    B --> C[Разбор Markdown]
    C --> D[HTML-документ]
    D --> E[Загрузка в WebView2]
    E --> F[Инициализация Mermaid]
    F --> G[Отрисовка SVG]
    G --> H[Проверка Ctrl+колесо]
    H --> I{Диаграмма увеличилась?}
    I -->|Да| J[Поведение корректно]
    I -->|Нет| K[Зафиксировать регрессию]
```

## Диаграмма последовательности

```mermaid
sequenceDiagram
    participant User as Пользователь
    participant TC as Total Commander
    participant Plugin as Markdown Viewer
    participant WebView as WebView2
    participant Mermaid as Mermaid

    User->>TC: Открывает .md в Lister
    TC->>Plugin: ListLoad
    Plugin->>WebView: Передаёт готовый HTML
    WebView->>Mermaid: Инициализация диаграмм
    Mermaid-->>WebView: SVG вместо блока кода
    User->>WebView: Ctrl+колесо
    WebView-->>User: Текст и диаграмма меняют масштаб
```

## Что проверить

- [ ] На месте блоков кода появились диаграммы, а не текст Mermaid.
- [ ] При увеличении масштаба обычный текст становится крупнее.
- [ ] При увеличении масштаба диаграммы тоже становятся крупнее.
- [ ] Если широкая диаграмма не помещается, её всё равно можно рассмотреть без обрезания важного текста.
