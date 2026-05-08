using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Markdig;
using Markdig.Extensions.AutoIdentifiers;
using Markdig.Extensions.Yaml;
using Markdig.Renderers;
using Markdig.Renderers.Html;
using Markdig.Renderers.Html.Inlines;
using Markdig.Syntax;
using Markdig.Syntax.Inlines;

namespace MarkdigNative;

public static class Lib
{
    static Lib()
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
    }

    /// <summary>
    /// GitHub-style :shortcode: → Unicode emoji (Markdig requires space before shortcode, so we pre-process).
    /// </summary>
    private static readonly Dictionary<string, string> GitHubEmojiShortcodes = new(StringComparer.OrdinalIgnoreCase)
    {
        { ":arrow_up:", "⬆️" },
        { ":white_check_mark:", "✅" },
        { ":negative_squared_cross_mark:", "❎" },
        { ":black_square_button:", "🔲" },
    };

    private static string ReplaceGitHubEmojiShortcodes(string text)
    {
        foreach (var kv in GitHubEmojiShortcodes)
            text = text.Replace(kv.Key, kv.Value);
        return text;
    }

    /// <summary>
    /// Replace :shortcode: with emoji only outside fenced code blocks (```), so examples stay literal.
    /// </summary>
    private static string ReplaceGitHubEmojiOutsideCodeBlocks(string source)
    {
        string[] parts = source.Split("```");
        for (int i = 0; i < parts.Length; i += 2)
            parts[i] = ReplaceGitHubEmojiShortcodes(parts[i]);
        return string.Join("```", parts);
    }

    // Шаблоны опасных HTML-блоков, которые нужно удалять вместе с содержимым.
    // Markdig только парсит тег как HtmlInline, а текст внутри (`alert(99)`)
    // отдаётся как обычный LiteralInline и попадает в HTML как видимый текст.
    private static readonly Regex[] DangerousHtmlBlockPatterns = new[]
    {
        new Regex(@"<script\b[^>]*>[\s\S]*?</script\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<style\b[^>]*>[\s\S]*?</style\s*>",  RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<noscript\b[^>]*>[\s\S]*?</noscript\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<iframe\b[^>]*>[\s\S]*?</iframe\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<object\b[^>]*>[\s\S]*?</object\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<embed\b[^>]*/?>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
        new Regex(@"<applet\b[^>]*>[\s\S]*?</applet\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled),
    };

    // Удаляет опасные HTML-блоки (script, style, iframe и т.п.) вместе с содержимым.
    // Применяется только к сегментам markdown ВНЕ fenced code blocks (между ```),
    // чтобы не задеть примеры HTML в документации.
    private static string StripDangerousHtmlBlocks(string source)
    {
        string[] parts = source.Split("```");
        for (int i = 0; i < parts.Length; i += 2)
        {
            foreach (var pattern in DangerousHtmlBlockPatterns)
                parts[i] = pattern.Replace(parts[i], "");
        }
        return string.Join("```", parts);
    }

    private sealed record CssCacheEntry(DateTime LastWriteTimeUtc, string Content);

    private static readonly ConcurrentDictionary<string, CssCacheEntry> CssCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, MarkdownPipeline> PipelineCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly Encoding Utf8Strict = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);
    private static readonly Encoding SystemAnsi = GetSystemAnsiEncoding();

    private const string CodeCopyButtonStyles = """
<style>
.mdv-code-block {
  position: relative;
}
.mdv-code-block > pre {
  padding-right: 3.75rem;
}
.mdv-code-copy-button {
  position: absolute;
  top: 8px;
  right: 8px;
  z-index: 2;
  display: flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 28px;
  padding: 0;
  border: 1px solid #d0d7de;
  border-radius: 6px;
  color: #57606a;
  background: rgba(246, 248, 250, 0.94);
  cursor: pointer;
  overflow: visible;
  opacity: 0;
  transition: opacity .12s ease, border-color .12s ease, color .12s ease, background-color .12s ease;
}
.mdv-code-block:hover .mdv-code-copy-button,
.mdv-code-copy-button:focus {
  opacity: 1;
}
.mdv-code-copy-button:hover {
  color: #24292f;
  border-color: #8c959f;
  background: #f3f4f6;
}
.mdv-code-copy-button::before {
  content: attr(data-mdv-tooltip);
  position: absolute;
  right: calc(100% + 10px);
  top: 50%;
  transform: translateY(-50%);
  padding: 5px 8px;
  border-radius: 6px;
  color: #f6f8fa;
  background: #24292f;
  font: 12px/18px -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  white-space: nowrap;
  pointer-events: none;
  opacity: 0;
  transition: opacity .12s ease;
}
.mdv-code-copy-button::after {
  content: "";
  position: absolute;
  right: calc(100% + 4px);
  top: 50%;
  transform: translateY(-50%);
  border: 6px solid transparent;
  border-left-color: #24292f;
  pointer-events: none;
  opacity: 0;
  transition: opacity .12s ease;
}
.mdv-code-copy-button:hover::before,
.mdv-code-copy-button:hover::after,
.mdv-code-copy-button:focus::before,
.mdv-code-copy-button:focus::after,
.mdv-code-copy-button.mdv-copied::before,
.mdv-code-copy-button.mdv-copied::after {
  opacity: 1;
}
.mdv-code-copy-button .octicon {
  width: 16px;
  height: 16px;
  fill: currentColor;
}
.mdv-code-copy-button .mdv-copy-check {
  display: none;
}
.mdv-code-copy-button.mdv-copied .mdv-copy-icon {
  display: none;
}
.mdv-code-copy-button.mdv-copied .mdv-copy-check {
  display: block;
}
.mdv-code-copy-button.mdv-copied,
.mdv-code-copy-button.mdv-copied:hover {
  color: #1a7f37;
  border-color: transparent;
  background: transparent;
}
@media (prefers-color-scheme: dark) {
  .mdv-code-copy-button {
    color: #8b949e;
    background: rgba(22, 27, 34, 0.94);
    border-color: #30363d;
  }
  .mdv-code-copy-button:hover {
    color: #e6edf3;
    border-color: #8b949e;
    background: #30363d;
  }
  .mdv-code-copy-button.mdv-copied,
  .mdv-code-copy-button.mdv-copied:hover {
    color: #3fb950;
    border-color: transparent;
    background: transparent;
  }
}
</style>
""";

    private const string CodeCopyButtonScript = """
<script>
(function(){
  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise(function(resolve, reject) {
      var textarea = document.createElement('textarea');
      textarea.value = text;
      textarea.setAttribute('readonly', '');
      textarea.style.position = 'fixed';
      textarea.style.left = '-1000px';
      textarea.style.top = '-1000px';
      document.body.appendChild(textarea);
      textarea.focus();
      textarea.select();
      try {
        if (document.execCommand('copy')) resolve();
        else reject(new Error('copy command failed'));
      } catch (err) {
        reject(err);
      } finally {
        document.body.removeChild(textarea);
      }
    });
  }

  function setButtonState(button, copied) {
    var original = button.getAttribute('data-mdv-label') || 'Copy';
    var copiedText = button.getAttribute('data-copy-feedback') || 'Copied!';
    button.classList.toggle('mdv-copied', copied);
    button.setAttribute('aria-label', copied ? copiedText : original);
    button.setAttribute('data-mdv-tooltip', copied ? copiedText : original);
    clearTimeout(button._mdvCopyTimer);
    button._mdvCopyTimer = setTimeout(function() {
      button.classList.remove('mdv-copied');
      button.setAttribute('aria-label', original);
      button.setAttribute('data-mdv-tooltip', original);
    }, 1200);
  }

  document.addEventListener('DOMContentLoaded', function() {
    document.querySelectorAll('pre > code:not(.language-mermaid)').forEach(function(code) {
      var pre = code.parentElement;
      if (!pre || pre.getAttribute('data-mdv-copy-ready') === '1') return;
      pre.setAttribute('data-mdv-copy-ready', '1');

      var wrapper = document.createElement('div');
      wrapper.className = 'mdv-code-block';
      pre.parentNode.insertBefore(wrapper, pre);
      wrapper.appendChild(pre);

      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'mdv-code-copy-button';
      button.setAttribute('data-mdv-label', 'Copy');
      button.setAttribute('data-copy-feedback', 'Copied!');
      button.setAttribute('data-mdv-tooltip', 'Copy');
      button.setAttribute('aria-label', 'Copy');
      button.innerHTML = '<svg aria-hidden="true" height="16" viewBox="0 0 16 16" version="1.1" width="16" class="octicon octicon-copy mdv-copy-icon"><path d="M0 6.75C0 5.784.784 5 1.75 5h1.5a.75.75 0 0 1 0 1.5h-1.5a.25.25 0 0 0-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 0 0 .25-.25v-1.5a.75.75 0 0 1 1.5 0v1.5A1.75 1.75 0 0 1 9.25 16h-7.5A1.75 1.75 0 0 1 0 14.25Z"></path><path d="M5 1.75C5 .784 5.784 0 6.75 0h7.5C15.216 0 16 .784 16 1.75v7.5A1.75 1.75 0 0 1 14.25 11h-7.5A1.75 1.75 0 0 1 5 9.25Zm1.75-.25a.25.25 0 0 0-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 0 0 .25-.25v-7.5a.25.25 0 0 0-.25-.25Z"></path></svg><svg aria-hidden="true" height="16" viewBox="0 0 16 16" version="1.1" width="16" class="octicon octicon-check mdv-copy-check"><path d="M13.78 4.22a.75.75 0 0 1 0 1.06l-7.25 7.25a.75.75 0 0 1-1.06 0L2.22 9.28a.751.751 0 0 1 .018-1.042.751.751 0 0 1 1.042-.018L6 10.94l6.72-6.72a.75.75 0 0 1 1.06 0Z"></path></svg>';
      button.addEventListener('click', function(event) {
        event.preventDefault();
        event.stopPropagation();
        copyText(code.textContent || '').then(function() {
          setButtonState(button, true);
        }, function() {
          setButtonState(button, false);
        });
      });
      wrapper.appendChild(button);
    });
  });
})();
</script>
""";

    private readonly record struct HtmlCacheKey(
        string FilenameKey,
        long MdLastWriteTicksUtc,
        long MdLength,
        string CssFileKey,
        long CssLastWriteTicksUtc,
        string ExtensionsKey
    );

    private const int HtmlCacheMaxEntries = 8;
    private const int HtmlCacheMaxCharsPerEntry = 2_000_000;
    private static readonly object HtmlCacheLock = new();
    private static readonly Dictionary<HtmlCacheKey, string> HtmlCache = new();
    private static readonly Dictionary<HtmlCacheKey, LinkedListNode<HtmlCacheKey>> HtmlCacheNodes = new();
    private static readonly LinkedList<HtmlCacheKey> HtmlCacheLru = new();

    internal static readonly Regex SafeIdPattern = new(
        @"^[\p{L}\p{N}_\-:.]+$",
        RegexOptions.Compiled);

    internal static readonly Regex AnchorOpenTagPattern = new(
        @"^<a\b(?=[^>]*\b(name|id)\s*=\s*([""'])([^""']*)\2)[^>]*>$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex AnchorCloseTagPattern = new(
        @"^</a\s*>$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex SelfClosingTagSuffix = new(
        @"/\s*>$",
        RegexOptions.Compiled);

    internal static readonly Regex HtmlHeadingBlockPattern = new(
        @"^\s*<(h[1-6])\b(?=[^>]*\bid\s*=\s*([""'])([^""']*)\2)[^>]*>([\s\S]*?)</\1\s*>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex StandaloneAnchorParagraphPattern = new(
        @"<p>\s*(<a\s+name=""[\p{L}\p{N}_\-:.]+""\s+id=""[\p{L}\p{N}_\-:.]+""></a>)\s*</p>",
        RegexOptions.Compiled);

    // ===== HTML allowlist расширение для GitHub-совместимых тегов =====

    // Inline void tags (без content): <br>, <br/>, <wbr>
    internal static readonly HashSet<string> SafeVoidInlineTags =
        new(StringComparer.OrdinalIgnoreCase) { "br", "wbr" };

    // Paired inline tags: <sub>X</sub>, <sup>X</sup>, <kbd>X</kbd>, <mark>X</mark>...
    internal static readonly HashSet<string> SafePairedInlineTags =
        new(StringComparer.OrdinalIgnoreCase)
        { "sub", "sup", "kbd", "mark", "ins", "del", "small", "abbr", "cite", "q" };

    // Inline-теги, разрешённые внутри <summary>:
    // включают существующие SafePairedInlineTags + расширение для типичных GitHub-форматов
    internal static readonly HashSet<string> SafeSummaryInlineTags =
        new(StringComparer.OrdinalIgnoreCase)
        {
            "b", "i", "strong", "em", "code",
            "sub", "sup", "kbd", "mark", "ins", "del", "small", "abbr", "cite", "q",
            "br", "wbr"
        };

    // Все теги для поиска через regex (любое имя — мы потом проверим через Contains)
    internal static readonly Regex AnyTagPattern = new(
        @"<(/?)(\w+)\b([^>]*)>",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex AnyVoidInlineTagPattern = new(
        @"^<(\w+)\b[^>]*/?>$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex AnyPairedOpenInlineTagPattern = new(
        @"^<(\w+)\b[^>]*>$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex AnyPairedCloseInlineTagPattern = new(
        @"^</(\w+)\s*>$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Block tag patterns: <hr>, <details>, <summary>
    internal static readonly Regex HtmlHrFragmentPattern = new(
        @"^\s*<hr\b[^>]*/?>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Compact full-block: <details ...>...inner...</details> в одном HtmlBlock
    // Группы: 1=attrs, 2=optional summary text, 3=inner после summary до </details>
    internal static readonly Regex HtmlDetailsFullBlockPattern = new(
        @"^\s*<details\b([^>]*)>\s*(?:<summary\b[^>]*>([\s\S]*?)</summary>)?([\s\S]*?)</details>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Open fragment: <details ...> [<summary>X</summary>] без </details>
    internal static readonly Regex HtmlDetailsOpenFragmentPattern = new(
        @"^\s*<details\b([^>]*)>\s*(?:<summary\b[^>]*>([\s\S]*?)</summary>)?\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex HtmlDetailsCloseFragmentPattern = new(
        @"^\s*</details\s*>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex HtmlSummaryFullFragmentPattern = new(
        @"^\s*<summary\b[^>]*>([\s\S]*?)</summary>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex HtmlSummaryOpenFragmentPattern = new(
        @"^\s*<summary\b[^>]*>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static readonly Regex HtmlSummaryCloseFragmentPattern = new(
        @"^\s*</summary\s*>\s*$",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Точная проверка boolean атрибута open (исключает data-open и т.п.)
    internal static readonly Regex DetailsOpenAttrPattern = new(
        @"(?:^|\s)open(?:\s|=|$)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static bool TryGetCachedHtml(HtmlCacheKey key, out string html)
    {
        lock (HtmlCacheLock)
        {
            if (!HtmlCache.TryGetValue(key, out html!))
                return false;

            if (HtmlCacheNodes.TryGetValue(key, out var node))
            {
                HtmlCacheLru.Remove(node);
                HtmlCacheLru.AddFirst(node);
            }
            return true;
        }
    }

    private static void PutCachedHtml(HtmlCacheKey key, string html)
    {
        if (html.Length > HtmlCacheMaxCharsPerEntry)
            return;

        lock (HtmlCacheLock)
        {
            if (HtmlCache.TryGetValue(key, out _))
            {
                HtmlCache[key] = html;
                if (HtmlCacheNodes.TryGetValue(key, out var existing))
                {
                    HtmlCacheLru.Remove(existing);
                    HtmlCacheLru.AddFirst(existing);
                }
                else
                {
                    var node = new LinkedListNode<HtmlCacheKey>(key);
                    HtmlCacheNodes[key] = node;
                    HtmlCacheLru.AddFirst(node);
                }
                return;
            }

            HtmlCache[key] = html;
            var newNode = new LinkedListNode<HtmlCacheKey>(key);
            HtmlCacheNodes[key] = newNode;
            HtmlCacheLru.AddFirst(newNode);

            while (HtmlCache.Count > HtmlCacheMaxEntries)
            {
                var last = HtmlCacheLru.Last;
                if (last is null)
                    break;
                HtmlCacheLru.RemoveLast();
                HtmlCache.Remove(last.Value);
                HtmlCacheNodes.Remove(last.Value);
            }
        }
    }

    private static Encoding GetSystemAnsiEncoding()
    {
        try
        {
            int cp = CultureInfo.CurrentCulture.TextInfo.ANSICodePage;
            return Encoding.GetEncoding(cp);
        }
        catch
        {
            return Encoding.UTF8;
        }
    }

    private static string ReadAllTextSequential(string filename)
    {
        try
        {
            using var fs = new FileStream(
                filename,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite,
                bufferSize: 64 * 1024,
                options: FileOptions.SequentialScan);

            using var sr = new StreamReader(fs, Utf8Strict, detectEncodingFromByteOrderMarks: true, bufferSize: 64 * 1024);
            return sr.ReadToEnd();
        }
        catch (DecoderFallbackException)
        {
            using var fs = new FileStream(
                filename,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite,
                bufferSize: 64 * 1024,
                options: FileOptions.SequentialScan);

            using var sr = new StreamReader(fs, SystemAnsi, detectEncodingFromByteOrderMarks: true, bufferSize: 64 * 1024);
            return sr.ReadToEnd();
        }
    }

    private static string GetCssContent(string? cssFile)
    {
        if (string.IsNullOrWhiteSpace(cssFile) || !File.Exists(cssFile))
            return "";

        DateTime lastWrite = File.GetLastWriteTimeUtc(cssFile);
        if (CssCache.TryGetValue(cssFile, out var cached) && cached.LastWriteTimeUtc == lastWrite)
            return cached.Content;

        string content = File.ReadAllText(cssFile);
        CssCache[cssFile] = new CssCacheEntry(lastWrite, content);
        return content;
    }

    private static MarkdownPipeline GetPipeline(string? extensions)
    {
        string key = (extensions ?? "").Trim();
        return PipelineCache.GetOrAdd(key, static exts =>
        {
            var builder = new MarkdownPipelineBuilder();

            builder.UseEmphasisExtras()
                   .UseAutoLinks()
                   .UseListExtras()
                   .UseCustomContainers()
                   .UseGenericAttributes();

            bool all = string.IsNullOrEmpty(exts) || exts.Contains("advanced", StringComparison.OrdinalIgnoreCase);
            bool allowHtml = exts.Contains("allowhtml", StringComparison.OrdinalIgnoreCase) ||
                             exts.Contains("unsafehtml", StringComparison.OrdinalIgnoreCase);

            if (!allowHtml)
                builder.Extensions.Add(new SafeRawHtmlAllowlistExtension());

            if (all || exts.Contains("pipetables", StringComparison.OrdinalIgnoreCase)) builder.UsePipeTables();
            if (all || exts.Contains("gridtables", StringComparison.OrdinalIgnoreCase)) builder.UseGridTables();
            if (all || exts.Contains("footnotes", StringComparison.OrdinalIgnoreCase)) builder.UseFootnotes();
            if (all || exts.Contains("citations", StringComparison.OrdinalIgnoreCase)) builder.UseCitations();
            if (all || exts.Contains("abbreviations", StringComparison.OrdinalIgnoreCase)) builder.UseAbbreviations();
            if (all || exts.Contains("emojis", StringComparison.OrdinalIgnoreCase)) builder.UseEmojiAndSmiley();
            if (all || exts.Contains("definitionlists", StringComparison.OrdinalIgnoreCase)) builder.UseDefinitionLists();
            if (all || exts.Contains("figures", StringComparison.OrdinalIgnoreCase)) builder.UseFigures();
            if (all || exts.Contains("mathematics", StringComparison.OrdinalIgnoreCase)) builder.UseMathematics();
            if (all || exts.Contains("bootstrap", StringComparison.OrdinalIgnoreCase)) builder.UseBootstrap();
            if (all || exts.Contains("medialinks", StringComparison.OrdinalIgnoreCase)) builder.UseMediaLinks();
            if (all || exts.Contains("smartypants", StringComparison.OrdinalIgnoreCase)) builder.UseSmartyPants();
            if (all || exts.Contains("autoidentifiers", StringComparison.OrdinalIgnoreCase))
                builder.UseAutoIdentifiers(AutoIdentifierOptions.GitHub);  // GitHub-style: сохраняет кириллицу в id
            if (all || exts.Contains("tasklists", StringComparison.OrdinalIgnoreCase)) builder.UseTaskLists();
            if (all || exts.Contains("diagrams", StringComparison.OrdinalIgnoreCase)) builder.UseDiagrams();
            if (all || exts.Contains("yaml", StringComparison.OrdinalIgnoreCase))
            {
                builder.UseYamlFrontMatter();
                builder.Extensions.Add(new GitHubStyleYamlExtension());
            }

            return builder.Build();
        });
    }

    [UnmanagedCallersOnly(EntryPoint = "ConvertMarkdownToHtml", CallConvs = [typeof(CallConvStdcall)])]
    public static unsafe IntPtr ConvertMarkdownToHtml(IntPtr filenamePtr, IntPtr cssFilePtr, IntPtr extensionsPtr)
    {
        try
        {
            string filename = Marshal.PtrToStringAnsi(filenamePtr) ?? "";
            string cssFile = Marshal.PtrToStringAnsi(cssFilePtr) ?? "";
            string extensions = Marshal.PtrToStringAnsi(extensionsPtr) ?? "";

            if (!File.Exists(filename))
            {
                return CreateNativeString("<html><body><h1>Error</h1><p>File not found</p></body></html>");
            }

            var mdInfo = new FileInfo(filename);
            DateTime mdLastWriteUtc = mdInfo.LastWriteTimeUtc;
            long mdLength = mdInfo.Length;
            DateTime cssLastWriteUtc = File.Exists(cssFile) ? File.GetLastWriteTimeUtc(cssFile) : default;

            string filenameKey = filename.ToLowerInvariant();
            string cssFileKey = (cssFile ?? "").ToLowerInvariant();
            string extensionsKey = (extensions ?? "").Trim().ToLowerInvariant();

            var cacheKey = new HtmlCacheKey(
                FilenameKey: filenameKey,
                MdLastWriteTicksUtc: mdLastWriteUtc.Ticks,
                MdLength: mdLength,
                CssFileKey: cssFileKey,
                CssLastWriteTicksUtc: cssLastWriteUtc.Ticks,
                ExtensionsKey: extensionsKey);

            if (TryGetCachedHtml(cacheKey, out string cachedHtml))
            {
                return CreateNativeString(cachedHtml);
            }

            string source = ReadAllTextSequential(filename);
            source = ReplaceGitHubEmojiOutsideCodeBlocks(source);

            // Удаляем опасные HTML-блоки целиком (тег + содержимое), если HTML не разрешён
            bool _allowHtmlForStrip = extensionsKey.Contains("allowhtml", StringComparison.OrdinalIgnoreCase)
                                   || extensionsKey.Contains("unsafehtml", StringComparison.OrdinalIgnoreCase);
            if (!_allowHtmlForStrip)
                source = StripDangerousHtmlBlocks(source);

            var pipeline = GetPipeline(extensionsKey);

            bool all = extensionsKey.Length == 0 || extensionsKey.Contains("advanced", StringComparison.OrdinalIgnoreCase);
            bool diagramsEnabled = all || extensionsKey.Contains("diagrams", StringComparison.OrdinalIgnoreCase);

            // Feature flags coming from C++ via extensions string ("|sh:on|sh-theme:dark"), explicit token compare to avoid substring false positives.
            var extensionTokens = extensionsKey.Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            bool syntaxHighlightEnabled = extensionTokens.Contains("sh:on", StringComparer.OrdinalIgnoreCase);
            bool syntaxHighlightDarkTheme = extensionTokens.Contains("sh-theme:dark", StringComparer.OrdinalIgnoreCase);

            string cssContent = GetCssContent(cssFile);

            var sb = new StringBuilder(source.Length + cssContent.Length + 2048);
            sb.AppendLine("<!DOCTYPE html>");
            sb.AppendLine("<html><head>");
            sb.AppendLine("<meta charset='utf-8'>");

            sb.Append("<style>");
            sb.Append(cssContent);
            sb.Append("</style>");
            sb.AppendLine(CodeCopyButtonStyles);
            if (diagramsEnabled)
            {
                sb.AppendLine("<script src='https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.min.js' defer></script>");
                sb.AppendLine("<script>window.addEventListener('load',function(){if(window.mermaid){try{mermaid.initialize({startOnLoad:true,theme:'default'});}catch(e){}}});</script>");
            }
            if (syntaxHighlightEnabled)
            {
                string hljsTheme = syntaxHighlightDarkTheme ? "github-dark" : "github";
                sb.Append("<link rel='stylesheet' href='https://assets.internal/vendor/hljs/").Append(hljsTheme).AppendLine(".min.css'>");
                sb.AppendLine("<link rel='stylesheet' href='https://assets.internal/vendor/hljs/themes/1c-overrides.css?v=5'>");
                sb.AppendLine("<script src='https://assets.internal/vendor/hljs/highlight.min.js' defer></script>");
                sb.AppendLine("<script src='https://assets.internal/vendor/hljs/languages/fsharp.min.js' defer></script>");
                sb.AppendLine("<script src='https://assets.internal/vendor/hljs/languages/1c.min.js' defer></script>");
                sb.AppendLine("<script>document.addEventListener('DOMContentLoaded',function(){if(!window.hljs)return;var aliasMap={'c#':'csharp','f#':'fsharp','1с':'1c','bsl':'1c'};document.querySelectorAll('pre code:not(.language-mermaid)').forEach(function(b){var lc=b.className.split(/\\s+/).find(function(c){return c.indexOf('language-')===0;});if(!lc)return;var lang=lc.substring(9).toLowerCase();lang=aliasMap[lang]||lang;if(!window.hljs.getLanguage(lang))return;try{var r=hljs.highlight(b.textContent||'',{language:lang,ignoreIllegals:true});b.innerHTML=r.value;b.classList.add('hljs');b.dataset.highlighted='yes';}catch(e){}});});</script>");
            }
            sb.AppendLine("</head>");
            sb.AppendLine("<body>");
            string renderedHtml = Markdown.ToHtml(source, pipeline);
            renderedHtml = StandaloneAnchorParagraphPattern.Replace(renderedHtml, "$1");
            sb.AppendLine(renderedHtml);
            sb.AppendLine(CodeCopyButtonScript);
            sb.AppendLine("<script>(function(){var n=function(s){return (s||'').replace(/-/g,' ').trim().toLowerCase();};var hs=Array.prototype.slice.call(document.querySelectorAll('h1,h2,h3,h4,h5,h6'));document.querySelectorAll('a[href^=\"#\"]').forEach(function(a){a.removeAttribute('title');var href=a.getAttribute('href');if(!href||href.length<2)return;var frag=href.slice(1);var decoded;try{decoded=decodeURIComponent(frag);}catch(e){decoded=frag;}if(document.getElementById(decoded)||document.getElementById(frag))return;var norm=n(decoded);if(!norm)return;for(var i=0;i<hs.length;i++){var h=hs[i];if(n(h.textContent)===norm){if(h.id){var span=document.createElement('span');span.id=decoded;h.parentNode.insertBefore(span,h);}else{h.id=decoded;}break;}}});})();</script>");
            sb.AppendLine("<script>document.addEventListener('click',function(e){var a=e.target;while(a&&a.tagName!=='A'){a=a.parentElement;}if(!a)return;var href=a.getAttribute('href');if(!href||href.length<2||href[0]!=='#')return;e.preventDefault();},true);</script>");
            sb.AppendLine("</body>");
            sb.AppendLine("</html>");

            string html = sb.ToString();
            PutCachedHtml(cacheKey, html);
            return CreateNativeString(html);
        }
        catch (Exception ex)
        {
            return CreateNativeString($"<html><body><h1>Error</h1><p>{ex.Message}</p></body></html>");
        }
    }

    private static IntPtr CreateNativeString(string text)
    {
        byte[] utf8Bytes = Encoding.UTF8.GetBytes(text);
        IntPtr nativeString = Marshal.AllocHGlobal(utf8Bytes.Length + 1);
        Marshal.Copy(utf8Bytes, 0, nativeString, utf8Bytes.Length);
        Marshal.WriteByte(nativeString, utf8Bytes.Length, 0);
        return nativeString;
    }

    [UnmanagedCallersOnly(EntryPoint = "FreeHtmlBuffer", CallConvs = [typeof(CallConvStdcall)])]
    public static void FreeHtmlBuffer(IntPtr buffer)
    {
        if (buffer != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    sealed class SafeAnchorHtmlInlineRenderer : HtmlObjectRenderer<HtmlInline>
    {
        protected override void Write(HtmlRenderer renderer, HtmlInline obj)
        {
            string tag = obj.Tag ?? string.Empty;

            // Закрывающий </a>
            if (AnchorCloseTagPattern.IsMatch(tag))
            {
                renderer.Write("</a>");
                return;
            }

            // <a name|id="x"> якорь
            var m = AnchorOpenTagPattern.Match(tag);
            if (m.Success)
            {
                string id = m.Groups[3].Value;
                if (!SafeIdPattern.IsMatch(id)) return;

                renderer.Write("<a name=\"").Write(id).Write("\" id=\"").Write(id).Write("\">");

                if (SelfClosingTagSuffix.IsMatch(tag))
                    renderer.Write("</a>");
                return;
            }

            // Self-closing void inline теги: <br>, <br/>, <wbr>
            var voidMatch = AnyVoidInlineTagPattern.Match(tag);
            if (voidMatch.Success && SafeVoidInlineTags.Contains(voidMatch.Groups[1].Value))
            {
                renderer.Write("<").Write(voidMatch.Groups[1].Value.ToLowerInvariant()).Write(">");
                return;
            }

            // Открывающие paired inline теги: <sub>, <sup>, <kbd>, <mark>, <ins>, <del>, <small>, <abbr>, <cite>, <q>
            var pairedOpen = AnyPairedOpenInlineTagPattern.Match(tag);
            if (pairedOpen.Success && SafePairedInlineTags.Contains(pairedOpen.Groups[1].Value))
            {
                renderer.Write("<").Write(pairedOpen.Groups[1].Value.ToLowerInvariant()).Write(">");
                return;
            }

            // Закрывающие paired inline теги
            var pairedClose = AnyPairedCloseInlineTagPattern.Match(tag);
            if (pairedClose.Success && SafePairedInlineTags.Contains(pairedClose.Groups[1].Value))
            {
                renderer.Write("</").Write(pairedClose.Groups[1].Value.ToLowerInvariant()).Write(">");
                return;
            }

            // Всё остальное (script, style, img, iframe, etc.) — стрипаем
        }
    }

    sealed class SafeHtmlBlockRenderer : HtmlObjectRenderer<HtmlBlock>
    {
        // Pipeline для рекурсивного Markdown.ToHtml внутри <details> (compact-mode).
        // Устанавливается в SafeRawHtmlAllowlistExtension.Setup.
        public MarkdownPipeline? Pipeline { get; set; }

        // Лимит глубины рекурсии compact-<details>. Защищает от stack overflow
        // на crafted markdown с глубокой вложенностью.
        // ThreadStatic — потому что каждый рекурсивный Markdown.ToHtml создаёт новый
        // SafeHtmlBlockRenderer instance, и нужен счётчик, общий для всей цепочки вызовов.
        [ThreadStatic]
        private static int _detailsRecursionDepth;
        private const int MaxDetailsRecursionDepth = 16;

        // Санитизация содержимого <summary>: разрешает только теги из SafeSummaryInlineTags,
        // канонизирует их (атрибуты отбрасываются), весь остальной текст HTML-экранируется.
        // Quote-aware tokenizer (учитывает ' и " в атрибутах при поиске конца тега).
        internal static string SanitizeSummaryInline(string content)
        {
            var sb = new StringBuilder();
            int pos = 0;
            while (pos < content.Length)
            {
                int lt = content.IndexOf('<', pos);
                if (lt < 0)
                {
                    AppendEscaped(sb, content, pos, content.Length - pos);
                    break;
                }
                if (lt > pos)
                    AppendEscaped(sb, content, pos, lt - pos);

                // Проверяем, что после '<' идёт буква (потенциальный тег) или '/'
                int nameStart = lt + 1;
                bool isClosing = false;
                if (nameStart < content.Length && content[nameStart] == '/')
                {
                    isClosing = true;
                    nameStart++;
                }
                if (nameStart >= content.Length || !char.IsLetter(content[nameStart]))
                {
                    // Не тег — экранируем '<' и продолжаем
                    sb.Append("&lt;");
                    pos = lt + 1;
                    continue;
                }

                // Извлекаем имя тега
                int nameEnd = nameStart;
                while (nameEnd < content.Length && (char.IsLetterOrDigit(content[nameEnd]) || content[nameEnd] == '-'))
                    nameEnd++;
                string tagName = content.Substring(nameStart, nameEnd - nameStart).ToLowerInvariant();

                // Quote-aware конец тега
                int tagEnd = FindTagEnd(content, nameEnd);
                if (tagEnd < 0)
                {
                    // Незакрытый тег — экранируем как литерал
                    AppendEscaped(sb, content, lt, content.Length - lt);
                    break;
                }

                if (SafeSummaryInlineTags.Contains(tagName))
                {
                    sb.Append(isClosing ? "</" : "<");
                    sb.Append(tagName);
                    sb.Append('>');
                }
                // else: тег вырезается

                pos = tagEnd + 1;
            }
            return sb.ToString();
        }

        private static void AppendEscaped(StringBuilder sb, string s, int start, int length)
        {
            int end = start + length;
            for (int i = start; i < end; i++)
            {
                char c = s[i];
                switch (c)
                {
                    case '&': sb.Append("&amp;"); break;
                    case '<': sb.Append("&lt;"); break;
                    case '>': sb.Append("&gt;"); break;
                    case '"': sb.Append("&quot;"); break;
                    default: sb.Append(c); break;
                }
            }
        }

        // Проверяет границу имени тега: следующий символ после имени тега должен быть
        // whitespace, '/', '>', или конец строки.
        internal static bool IsTagBoundaryAt(string s, int pos)
        {
            if (pos >= s.Length) return true;
            char c = s[pos];
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>';
        }

        // Проверяет, что в позиции pos начинается тег с заданным именем (case-insensitive)
        // и правильной границей. isClose=true для проверки закрывающего </name>.
        internal static bool IsTagAt(string s, int pos, string name, bool isClose)
        {
            int prefixLen = name.Length + (isClose ? 2 : 1); // < + name или </ + name
            if (pos + prefixLen > s.Length) return false;
            string expected = isClose ? "</" + name : "<" + name;
            if (string.Compare(s, pos, expected, 0, prefixLen, StringComparison.OrdinalIgnoreCase) != 0)
                return false;
            return IsTagBoundaryAt(s, pos + prefixLen);
        }

        // Извлекает <summary>...</summary> начиная с позиции from (quote-aware).
        // Возвращает (success, summary_inner, position_after_close).
        internal static (bool ok, string summary, int consumedTo) TryExtractSummaryAt(string s, int from)
        {
            if (!IsTagAt(s, from, "summary", false))
                return (false, "", from);

            int openEnd = FindTagEnd(s, from + 8);
            if (openEnd < 0) return (false, "", from);

            string attrs = s.Substring(from + 8, openEnd - (from + 8)).TrimEnd();
            // Self-closing <summary/> — пустой summary
            if (attrs.EndsWith("/"))
                return (true, "", openEnd + 1);

            // Ищем </summary> с правильной границей
            int pos = openEnd + 1;
            int closeStart = -1;
            while (pos < s.Length)
            {
                int lt = s.IndexOf('<', pos);
                if (lt < 0) return (false, "", from);
                if (IsTagAt(s, lt, "summary", true))
                {
                    closeStart = lt;
                    break;
                }
                pos = lt + 1;
            }
            if (closeStart < 0) return (false, "", from);

            int closeEnd = FindTagEnd(s, closeStart + 9);
            if (closeEnd < 0) return (false, "", from);

            string summary = s.Substring(openEnd + 1, closeStart - (openEnd + 1));
            return (true, summary, closeEnd + 1);
        }

        // Quote-aware поиск конца HTML-тега: пропускает '>' внутри одинарных и двойных кавычек.
        // Возвращает позицию '>' или -1 если не найдено.
        internal static int FindTagEnd(string s, int after)
        {
            char quote = '\0';
            for (int i = after; i < s.Length; i++)
            {
                char c = s[i];
                if (quote != '\0')
                {
                    if (c == quote) quote = '\0';
                }
                else
                {
                    if (c == '"' || c == '\'') quote = c;
                    else if (c == '>') return i;
                }
            }
            return -1;
        }

        // Проверяет, что в позиции pos строки s начинается тег <details или </details
        // с правильной границей имени тега. Исключает <details-extra>, <detailsfoo> и т.п.
        // Возвращает: 0 = не details тег, 1 = открывающий <details, -1 = закрывающий </details.
        internal static int DetailsTagKindAt(string s, int pos)
        {
            if (IsTagAt(s, pos, "details", false)) return 1;
            if (IsTagAt(s, pos, "details", true)) return -1;
            return 0;
        }

        // Проверяет, что позиция pos находится в начале строки (с indent <= 3 пробелов).
        // Используется для CommonMark-style fenced code: fence должен быть в начале строки.
        internal static bool IsAtLineStart(string s, int pos)
        {
            int lineStart = pos;
            while (lineStart > 0 && s[lineStart - 1] != '\n') lineStart--;
            int indent = 0;
            int p = lineStart;
            while (p < pos && (s[p] == ' ') && indent < 4)
            {
                indent++;
                p++;
            }
            return p == pos && indent <= 3;
        }

        // Находит границы fenced code blocks (``` или ~~~) и inline code (`...`) для skip-маски.
        // Возвращает упорядоченный список (start, end) — pos-ы, которые НЕ нужно сканировать как HTML.
        internal static List<(int start, int end)> FindCodeRegions(string s)
        {
            var regions = new List<(int, int)>();
            int i = 0;
            while (i < s.Length)
            {
                // Fenced ``` или ~~~ — opening fence ДОЛЖЕН быть в начале строки (с indent <= 3)
                if (i + 3 <= s.Length && (s[i] == '`' || s[i] == '~') && IsAtLineStart(s, i))
                {
                    char fence = s[i];
                    int run = 0;
                    while (i + run < s.Length && s[i + run] == fence) run++;
                    if (run >= 3)
                    {
                        // Конец строки fence
                        int eol = s.IndexOf('\n', i);
                        if (eol < 0) { i++; continue; }

                        // Ищем закрывающий fence в начале строки, не менее run символов
                        int searchFrom = eol + 1;
                        int closePos = -1;
                        while (searchFrom < s.Length)
                        {
                            int next = s.IndexOf(fence, searchFrom);
                            if (next < 0) break;
                            int lineStart = next;
                            while (lineStart > 0 && s[lineStart - 1] != '\n') lineStart--;
                            int indent = 0;
                            int p = lineStart;
                            while (p < next && s[p] == ' ' && indent < 4) { indent++; p++; }
                            if (p == next && indent <= 3)
                            {
                                int lineCloseRun = 0;
                                int q = next;
                                while (q < s.Length && s[q] == fence) { lineCloseRun++; q++; }
                                if (lineCloseRun >= run)
                                {
                                    closePos = q;
                                    break;
                                }
                            }
                            searchFrom = next + 1;
                        }
                        if (closePos < 0) closePos = s.Length;
                        regions.Add((i, closePos));
                        i = closePos;
                        continue;
                    }
                }
                // Inline code: одиночная или двойная backtick (может быть в любой позиции строки)
                if (s[i] == '`')
                {
                    int run = 0;
                    while (i + run < s.Length && s[i + run] == '`') run++;
                    if (run < 3)
                    {
                        int closeRun = s.IndexOf(new string('`', run), i + run);
                        if (closeRun < 0) { i++; continue; }
                        regions.Add((i, closeRun + run));
                        i = closeRun + run;
                        continue;
                    }
                }
                i++;
            }
            return regions;
        }

        // Проверяет, попадает ли позиция в один из code regions.
        internal static bool IsInsideCode(List<(int start, int end)> regions, int pos)
        {
            foreach (var r in regions)
                if (pos >= r.start && pos < r.end) return true;
            return false;
        }

        // Находит следующий details-тег (любого вида) начиная с pos, пропуская code regions.
        // Возвращает (-1, 0) если не найден.
        internal static (int pos, int kind) FindNextDetailsTag(string s, int from, List<(int start, int end)> codeRegions)
        {
            int pos = from;
            while (pos < s.Length)
            {
                int lt = s.IndexOf('<', pos);
                if (lt < 0) return (-1, 0);
                if (IsInsideCode(codeRegions, lt))
                {
                    // Перепрыгиваем за конец code region
                    foreach (var r in codeRegions)
                    {
                        if (lt >= r.start && lt < r.end) { pos = r.end; goto NextIter; }
                    }
                    pos = lt + 1;
                    continue;
                }
                int kind = DetailsTagKindAt(s, lt);
                if (kind != 0) return (lt, kind);
                pos = lt + 1;
                NextIter:;
            }
            return (-1, 0);
        }

        // Balanced-парсер для одного <details>...</details> с поддержкой вложенности.
        // Учитывает кавычки в атрибутах, границы имени тега, self-closing варианты,
        // и пропускает details-теги внутри code-блоков.
        // Возвращает позицию ПОСЛЕ закрывающего тега для последующего парсинга соседних блоков.
        internal static (bool ok, string attrs, string? summary, string inner, int consumedTo) TryParseOneDetailsBlock(string content, int startPos)
        {
            // Должен начинаться с <details с правильной границей
            if (DetailsTagKindAt(content, startPos) != 1)
                return (false, "", null, "", startPos);

            // Quote-aware конец открывающего тега
            int gt = FindTagEnd(content, startPos + 8);
            if (gt < 0) return (false, "", null, "", startPos);
            string attrs = content.Substring(startPos + 8, gt - (startPos + 8));

            // Self-closing <details/> — пустой блок без содержимого
            string attrsTrimmed = attrs.TrimEnd();
            if (attrsTrimmed.EndsWith("/"))
            {
                return (true, attrsTrimmed.Substring(0, attrsTrimmed.Length - 1).TrimEnd(), null, "", gt + 1);
            }

            // Balanced walk с пропуском code-блоков
            var codeRegions = FindCodeRegions(content);

            int depth = 1;
            int pos = gt + 1;
            int closeStart = -1;
            int closeEnd = -1;

            while (pos < content.Length && depth > 0)
            {
                var (tagPos, kind) = FindNextDetailsTag(content, pos, codeRegions);
                if (tagPos < 0)
                    return (false, "", null, "", startPos); // unbalanced

                int tagEnd = FindTagEnd(content, tagPos + (kind > 0 ? 8 : 9));
                if (tagEnd < 0) return (false, "", null, "", startPos);

                if (kind > 0)
                {
                    // Открывающий — проверяем self-closing
                    string innerAttrs = content.Substring(tagPos + 8, tagEnd - (tagPos + 8)).TrimEnd();
                    if (!innerAttrs.EndsWith("/"))
                        depth++;
                    // Если self-closing — depth не меняется (это void-тег)
                    pos = tagEnd + 1;
                }
                else
                {
                    depth--;
                    if (depth == 0)
                    {
                        closeStart = tagPos;
                        closeEnd = tagEnd;
                    }
                    pos = tagEnd + 1;
                }
            }

            if (closeStart < 0) return (false, "", null, "", startPos);

            string body = content.Substring(gt + 1, closeStart - gt - 1).Trim();

            // Опциональный <summary>...</summary> в начале body (quote-aware)
            string? summary = null;
            string inner = body;
            var summaryExtract = TryExtractSummaryAt(body, 0);
            if (summaryExtract.ok)
            {
                summary = summaryExtract.summary;
                inner = body.Substring(summaryExtract.consumedTo).Trim();
            }

            return (true, attrs, summary, inner, closeEnd + 1);
        }

        // Помогает emit'ить один уже распарсенный <details> блок в выходной поток.
        private void EmitDetails(HtmlRenderer renderer, string attrs, string? summary, string inner)
        {
            bool isOpen = DetailsOpenAttrPattern.IsMatch(attrs);
            renderer.Write(isOpen ? "<details open>" : "<details>");

            if (summary != null)
            {
                renderer.Write("<summary>");
                renderer.Write(SanitizeSummaryInline(summary));
                renderer.Write("</summary>");
            }

            if (!string.IsNullOrEmpty(inner))
            {
                if (Pipeline != null && _detailsRecursionDepth < MaxDetailsRecursionDepth)
                {
                    _detailsRecursionDepth++;
                    try
                    {
                        renderer.Write(Markdown.ToHtml(inner, Pipeline));
                    }
                    finally
                    {
                        _detailsRecursionDepth--;
                    }
                }
                else
                {
                    // Превышен лимит глубины ИЛИ Pipeline не установлен — fallback в безопасный escape
                    renderer.Write("<p>");
                    renderer.Write(SanitizeSummaryInline(inner));
                    renderer.Write("</p>");
                }
            }

            renderer.Write("</details>").WriteLine();
        }

        protected override void Write(HtmlRenderer renderer, HtmlBlock obj)
        {
            var lines = obj.Lines.Lines;
            var sb = new StringBuilder();
            for (int i = 0; i < obj.Lines.Count; i++)
                sb.AppendLine(lines[i].Slice.ToString());
            string content = sb.ToString().Trim();

            // <hN id="x">text</hN>
            var hMatch = HtmlHeadingBlockPattern.Match(content);
            if (hMatch.Success)
            {
                string level = hMatch.Groups[1].Value.ToLowerInvariant();
                string id = hMatch.Groups[3].Value;
                string inner = hMatch.Groups[4].Value.Trim();

                if (!SafeIdPattern.IsMatch(id)) return;

                renderer.Write("<").Write(level).Write(" id=\"").Write(id).Write("\">");
                renderer.WriteEscape(inner);
                renderer.Write("</").Write(level).Write(">").WriteLine();
                return;
            }

            // <hr>
            if (HtmlHrFragmentPattern.IsMatch(content))
            {
                renderer.Write("<hr/>").WriteLine();
                return;
            }

            // Compact full-block: один или несколько <details>...</details> в одном HtmlBlock.
            // Использует balanced-парсер для вложенности + цикл для соседних блоков.
            // Должен проверяться ПЕРЕД open-fragment regex.
            {
                int pos = 0;
                var blocks = new List<(string attrs, string? summary, string inner)>();

                while (pos < content.Length)
                {
                    // Пропускаем whitespace между блоками
                    while (pos < content.Length && char.IsWhiteSpace(content[pos])) pos++;
                    if (pos >= content.Length) break;

                    // Ожидаем <details — иначе это не последовательность details блоков
                    if (string.Compare(content, pos, "<details", 0, 8, StringComparison.OrdinalIgnoreCase) != 0)
                    {
                        blocks.Clear();
                        break;
                    }

                    var chunk = TryParseOneDetailsBlock(content, pos);
                    if (!chunk.ok)
                    {
                        blocks.Clear();
                        break;
                    }
                    blocks.Add((chunk.attrs, chunk.summary, chunk.inner));
                    pos = chunk.consumedTo;
                }

                if (blocks.Count > 0)
                {
                    foreach (var b in blocks)
                        EmitDetails(renderer, b.attrs, b.summary, b.inner);
                    return;
                }
            }

            // <details ...> [+ optional <summary>X</summary>] (fragment mode, без </details>)
            // Quote-aware вместо regex
            {
                string trimmed = content.TrimStart();
                int trimOffset = content.Length - trimmed.Length;
                if (DetailsTagKindAt(trimmed, 0) == 1)
                {
                    int gt = FindTagEnd(trimmed, 8);
                    if (gt > 0)
                    {
                        string attrs = trimmed.Substring(8, gt - 8);
                        // Не self-closing (это бы парсилось как compact full block выше)
                        // и нет </details> в этом блоке (иначе парсилось бы как compact)
                        // — проверяем что после opening идёт либо whitespace до конца, либо <summary>...</summary>
                        string after = trimmed.Substring(gt + 1).TrimStart();
                        bool isOpen = DetailsOpenAttrPattern.IsMatch(attrs);

                        // Проверяем что нет </details> в этом блоке
                        bool hasClose = false;
                        int searchPos = gt + 1;
                        while (searchPos < trimmed.Length)
                        {
                            int lt = trimmed.IndexOf('<', searchPos);
                            if (lt < 0) break;
                            if (DetailsTagKindAt(trimmed, lt) == -1) { hasClose = true; break; }
                            searchPos = lt + 1;
                        }

                        if (!hasClose)
                        {
                            // Опциональный <summary>...</summary> сразу после opening
                            string? summary = null;
                            string trailingAfter = after;
                            if (after.Length > 0)
                            {
                                var sm = TryExtractSummaryAt(after, 0);
                                if (sm.ok)
                                {
                                    summary = sm.summary;
                                    trailingAfter = after.Substring(sm.consumedTo).TrimStart();
                                }
                            }
                            // Допустимо: после opening — только summary и/или whitespace
                            if (trailingAfter.Length == 0)
                            {
                                renderer.Write(isOpen ? "<details open>" : "<details>");
                                if (summary != null)
                                {
                                    renderer.Write("<summary>");
                                    renderer.Write(SanitizeSummaryInline(summary));
                                    renderer.Write("</summary>");
                                }
                                renderer.WriteLine();
                                return;
                            }
                        }
                    }
                }
            }

            // </details>
            {
                string trimmed = content.Trim();
                if (DetailsTagKindAt(trimmed, 0) == -1)
                {
                    int gt = FindTagEnd(trimmed, 9);
                    if (gt > 0 && trimmed.Substring(gt + 1).Trim().Length == 0)
                    {
                        renderer.Write("</details>").WriteLine();
                        return;
                    }
                }
            }

            // <summary>full</summary> — quote-aware
            {
                string trimmed = content.Trim();
                var sm = TryExtractSummaryAt(trimmed, 0);
                if (sm.ok && sm.consumedTo == trimmed.Length)
                {
                    renderer.Write("<summary>");
                    renderer.Write(SanitizeSummaryInline(sm.summary));
                    renderer.Write("</summary>").WriteLine();
                    return;
                }
            }

            // <summary> open (без закрытия)
            {
                string trimmed = content.Trim();
                if (IsTagAt(trimmed, 0, "summary", false))
                {
                    int gt = FindTagEnd(trimmed, 8);
                    if (gt > 0 && trimmed.Substring(gt + 1).Trim().Length == 0)
                    {
                        renderer.Write("<summary>").WriteLine();
                        return;
                    }
                }
            }

            // </summary>
            {
                string trimmed = content.Trim();
                if (IsTagAt(trimmed, 0, "summary", true))
                {
                    int gt = FindTagEnd(trimmed, 9);
                    if (gt > 0 && trimmed.Substring(gt + 1).Trim().Length == 0)
                    {
                        renderer.Write("</summary>").WriteLine();
                        return;
                    }
                }
            }

            // Не наш формат — стрипаем (как DisableHtml)
        }
    }

    sealed class SafeRawHtmlAllowlistExtension : IMarkdownExtension
    {
        public void Setup(MarkdownPipelineBuilder pipeline) { }

        public void Setup(MarkdownPipeline pipeline, IMarkdownRenderer renderer)
        {
            if (renderer is not HtmlRenderer h) return;

            var inlineExisting = h.ObjectRenderers.FindExact<HtmlInlineRenderer>();
            if (inlineExisting != null) h.ObjectRenderers.Remove(inlineExisting);
            h.ObjectRenderers.Add(new SafeAnchorHtmlInlineRenderer());

            var blockExisting = h.ObjectRenderers.FindExact<HtmlBlockRenderer>();
            if (blockExisting != null) h.ObjectRenderers.Remove(blockExisting);
            // Pipeline передаётся для рекурсивного Markdown.ToHtml внутри <details> compact-mode
            h.ObjectRenderers.Add(new SafeHtmlBlockRenderer { Pipeline = pipeline });
        }
    }

    sealed class GitHubStyleYamlFrontMatterRenderer : HtmlObjectRenderer<YamlFrontMatterBlock>
    {
        protected override void Write(HtmlRenderer renderer, YamlFrontMatterBlock obj)
        {
            renderer.Write("<table class=\"markdown-frontmatter\"><tbody>");

            for (int i = 0; i < obj.Lines.Count; i++)
            {
                string line = obj.Lines.Lines[i].Slice.ToString();
                string trimmed = line.Trim();

                // Пропускаем разделители "---" и пустые строки
                if (string.IsNullOrEmpty(trimmed) || trimmed == "---") continue;

                int colon = trimmed.IndexOf(':');
                if (colon <= 0) continue;

                string key = trimmed.Substring(0, colon).Trim();
                string value = trimmed.Substring(colon + 1).Trim();

                // Снимаем парные кавычки если есть
                if (value.Length >= 2 &&
                    (value[0] == '"' || value[0] == '\'') &&
                    value[value.Length - 1] == value[0])
                {
                    value = value.Substring(1, value.Length - 2);
                }

                renderer.Write("<tr><th>");
                renderer.WriteEscape(key);
                renderer.Write("</th><td>");
                renderer.WriteEscape(value);
                renderer.Write("</td></tr>");
            }

            renderer.Write("</tbody></table>");
            renderer.WriteLine();
        }
    }

    sealed class GitHubStyleYamlExtension : IMarkdownExtension
    {
        public void Setup(MarkdownPipelineBuilder pipeline) { }

        public void Setup(MarkdownPipeline pipeline, IMarkdownRenderer renderer)
        {
            if (renderer is not HtmlRenderer h) return;

            // Markdig 0.40.0 регистрирует пустой YamlFrontMatterHtmlRenderer (стрипает блок).
            // Заменяем на свой, рендерящий как GitHub-style таблицу.
            var existing = h.ObjectRenderers.FindExact<YamlFrontMatterHtmlRenderer>();
            if (existing != null) h.ObjectRenderers.Remove(existing);

            // КРИТИЧНО: Insert(0, ...) — YamlFrontMatterBlock наследуется от CodeBlock.
            // Если добавить в конец списка, CodeBlockRenderer перехватит блок и отрендерит как <pre><code>.
            // Insert в начало гарантирует, что наш renderer проверится первым.
            h.ObjectRenderers.Insert(0, new GitHubStyleYamlFrontMatterRenderer());
        }
    }
}
