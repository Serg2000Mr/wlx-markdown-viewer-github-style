# Markdown Lister Plugin for Total Commander

A plugin for viewing Markdown files in Total Commander with modern features and GitHub-like appearance.

![Plugin Screenshot]("https://github.com/user-attachments/assets/6eaaabb2-b2b2-40c1-bfb2-254bb1e133ad")

## ✨ Key Features

- **GitHub-style appearance** - Matches GitHub rendering exactly, considered the gold standard
- **Mermaid.js diagrams** - Full support for flowcharts, sequence diagrams, ER diagrams and more
- **Images** - Display embedded images and external images via URL
- **Tables** - Full pipe-table support with alignment
- **Emoji** - GitHub-style emoji via shortcodes (`:emoji_name:`)
- **Task lists** - Interactive checkboxes for task tracking
- **Code highlighting** - Syntax highlighting for code blocks
- **Math formulas** - LaTeX-style mathematical expressions

## 🚀 Key Advantages

- **No external dependencies** - No .NET Runtime installation required
- **Modern WebView2 engine** - Based on Chromium instead of outdated Internet Explorer
- **Fast and lightweight** - Optimized for performance

## 📦 Installation

1. Download the archive from [Releases](https://github.com/Serg2000Mr/wlx-markdown-viewer/releases)
2. Open the archive in Total Commander
3. Confirm plugin installation

## 🧪 Testing

You can test the plugin with example files in the `Примеры` folder.

## 🎨 Themes

The plugin supports multiple CSS themes:
- **GitHub** (default) - Classic GitHub appearance
- **GitHub Dark** - Dark theme for GitHub
- **GitHub Retro** - Retro GitHub style
- **Air** - Clean and minimalist
- **Modest** - Simple and elegant
- **Splendor** - Rich and colorful

## ⚙️ Configuration

Configuration is done through the `MarkdownView.ini` file:

```ini
[Settings]
CssFile=github.css
EnableMermaid=1
EnableMath=1
EnableEmoji=1
```

## 🔧 Technical Details

### Architecture

1. **Rendering Engine (C# Native AOT)**:
   - Location: `MarkdigNative/`
   - Library: [Markdig](https://github.com/xoofx/markdig)
   - Technology: .NET 8 Native AOT (Ahead-of-Time compilation)
   - Output: `MarkdigNative-x64.dll` (standalone native DLL with no .NET runtime requirement)
   - Exports: `ConvertMarkdownToHtml`, `FreeHtmlBuffer`

2. **Bridge Layer (Pure C++)**:
   - Location: `Markdown/`
   - Role: Acts as a middleman between the Lister plugin and the AOT engine
   - Loading: Uses dynamic loading (`LoadLibraryW`) to find the AOT DLL relative to its own path
   - Compatibility: Replaces the old C++/CLI implementation to remove managed code dependencies

3. **Lister Plugin (C++)**:
   - Location: `MarkdownView/`
   - Role: Total Commander interface implementation
   - Technology: Pure Win32 C++ with WebView2
   - Features: File loading, WebView2 integration, configuration management

### Build Requirements

- Visual Studio 2022 with C++ workload
- .NET 8 SDK
- WebView2 SDK (included via NuGet)

### Build Process

```bash
# Build all components
BuildAll.bat

# Or build individually:
# 1. Build .NET AOT component
dotnet publish MarkdigNative/MarkdigNative.csproj -c Release

# 2. Build C++ components
msbuild MarkdownView.sln /p:Configuration=Release /p:Platform=x64
```

## 📋 System Requirements

- Windows 10 version 1903 or later
- WebView2 Runtime (usually pre-installed on modern Windows)
- Total Commander 9.0 or later

## 🤝 Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## 📄 License

This project is licensed under the MIT License - see the original repository for details.

## 🙏 Acknowledgments

- [Markdig](https://github.com/xoofx/markdig) - Excellent Markdown processor for .NET
- [Mermaid.js](https://mermaid.js.org/) - Diagram and flowchart generation
- [GitHub](https://github.com) - For the reference CSS styles
- Original [wlx-markdown-viewer](https://github.com/rg-software/wlx-markdown-viewer) project

## 📞 Support

If you encounter any issues or have questions, please create an issue in this repository.
