using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;
using Markdig;

namespace MarkdigNative;

public static class Lib
{
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

    private sealed record CssCacheEntry(DateTime LastWriteTimeUtc, string Content);

    private static readonly ConcurrentDictionary<string, CssCacheEntry> CssCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, MarkdownPipeline> PipelineCache = new(StringComparer.OrdinalIgnoreCase);

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

    private static string ReadAllTextSequential(string filename)
    {
        using var fs = new FileStream(
            filename,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite,
            bufferSize: 64 * 1024,
            options: FileOptions.SequentialScan);

        using var sr = new StreamReader(fs, Encoding.UTF8, detectEncodingFromByteOrderMarks: true, bufferSize: 64 * 1024);
        return sr.ReadToEnd();
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
            if (all || exts.Contains("autoidentifiers", StringComparison.OrdinalIgnoreCase)) builder.UseAutoIdentifiers();
            if (all || exts.Contains("tasklists", StringComparison.OrdinalIgnoreCase)) builder.UseTaskLists();
            if (all || exts.Contains("diagrams", StringComparison.OrdinalIgnoreCase)) builder.UseDiagrams();
            if (all || exts.Contains("yaml", StringComparison.OrdinalIgnoreCase)) builder.UseYamlFrontMatter();

            return builder.Build();
        });
    }

    [UnmanagedCallersOnly(EntryPoint = "ConvertMarkdownToHtml")]
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

            var pipeline = GetPipeline(extensions);

            bool all = string.IsNullOrEmpty(extensions) || extensions.Contains("advanced", StringComparison.OrdinalIgnoreCase);
            bool diagramsEnabled = all || extensions.Contains("diagrams", StringComparison.OrdinalIgnoreCase);
            string cssContent = GetCssContent(cssFile);

            var sb = new StringBuilder(source.Length + cssContent.Length + 2048);
            sb.AppendLine("<!DOCTYPE html>");
            sb.AppendLine("<html><head>");
            sb.AppendLine("<meta charset='utf-8'>");

            sb.Append("<style>");
            sb.Append(cssContent);
            sb.Append("</style>");
            if (diagramsEnabled)
            {
                sb.AppendLine("<script src='https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.min.js' defer></script>");
                sb.AppendLine("<script>window.addEventListener('load',function(){if(window.mermaid){try{mermaid.initialize({startOnLoad:true,theme:'default'});}catch(e){}}});</script>");
            }
            sb.AppendLine("</head>");
            sb.AppendLine("<body>");
            sb.AppendLine(Markdown.ToHtml(source, pipeline));
            sb.AppendLine("<script>(function(){var n=function(s){return (s||'').replace(/-/g,' ').trim().toLowerCase();};var hs=Array.prototype.slice.call(document.querySelectorAll('h1,h2,h3,h4,h5,h6'));document.querySelectorAll('a[href^=\"#\"]').forEach(function(a){a.removeAttribute('title');var href=a.getAttribute('href');if(!href||href.length<2)return;var frag=href.slice(1);var decoded;try{decoded=decodeURIComponent(frag);}catch(e){decoded=frag;}if(document.getElementById(decoded)||document.getElementById(frag))return;var norm=n(decoded);if(!norm)return;for(var i=0;i<hs.length;i++){var h=hs[i];if(n(h.textContent)===norm){if(h.id){var span=document.createElement('span');span.id=decoded;h.parentNode.insertBefore(span,h);}else{h.id=decoded;}break;}}});})();</script>");
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

    [UnmanagedCallersOnly(EntryPoint = "FreeHtmlBuffer")]
    public static void FreeHtmlBuffer(IntPtr buffer)
    {
        if (buffer != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
