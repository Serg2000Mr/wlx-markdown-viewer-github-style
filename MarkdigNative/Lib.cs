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

            string source = File.ReadAllText(filename);
            source = ReplaceGitHubEmojiOutsideCodeBlocks(source);
            
            var builder = new MarkdownPipelineBuilder();
            
            // Default extensions (often used)
            builder.UseEmphasisExtras()
                   .UseAutoLinks()
                   .UseListExtras()
                   .UseCustomContainers()
                   .UseGenericAttributes();
            
            // Conditional extensions based on the extensions string
            bool all = string.IsNullOrEmpty(extensions) || extensions.Contains("advanced", StringComparison.OrdinalIgnoreCase);

            if (all || extensions.Contains("pipetables", StringComparison.OrdinalIgnoreCase)) builder.UsePipeTables();
            if (all || extensions.Contains("gridtables", StringComparison.OrdinalIgnoreCase)) builder.UseGridTables();
            if (all || extensions.Contains("footnotes", StringComparison.OrdinalIgnoreCase)) builder.UseFootnotes();
            if (all || extensions.Contains("citations", StringComparison.OrdinalIgnoreCase)) builder.UseCitations();
            if (all || extensions.Contains("abbreviations", StringComparison.OrdinalIgnoreCase)) builder.UseAbbreviations();
            if (all || extensions.Contains("emojis", StringComparison.OrdinalIgnoreCase)) builder.UseEmojiAndSmiley();
            if (all || extensions.Contains("definitionlists", StringComparison.OrdinalIgnoreCase)) builder.UseDefinitionLists();
            if (all || extensions.Contains("figures", StringComparison.OrdinalIgnoreCase)) builder.UseFigures();
            if (all || extensions.Contains("mathematics", StringComparison.OrdinalIgnoreCase)) builder.UseMathematics();
            if (all || extensions.Contains("bootstrap", StringComparison.OrdinalIgnoreCase)) builder.UseBootstrap();
            if (all || extensions.Contains("medialinks", StringComparison.OrdinalIgnoreCase)) builder.UseMediaLinks();
            if (all || extensions.Contains("smartypants", StringComparison.OrdinalIgnoreCase)) builder.UseSmartyPants();
            if (all || extensions.Contains("autoidentifiers", StringComparison.OrdinalIgnoreCase)) builder.UseAutoIdentifiers();
            if (all || extensions.Contains("tasklists", StringComparison.OrdinalIgnoreCase)) builder.UseTaskLists();
            if (all || extensions.Contains("diagrams", StringComparison.OrdinalIgnoreCase)) builder.UseDiagrams();
            if (all || extensions.Contains("yaml", StringComparison.OrdinalIgnoreCase)) builder.UseYamlFrontMatter();

            var pipeline = builder.Build();

            var sb = new StringBuilder(1000);
            sb.AppendLine("<!DOCTYPE html>");
            sb.AppendLine("<html><head>");
            sb.AppendLine("<meta charset='utf-8'>");

            sb.Append("<style>");
            if (File.Exists(cssFile))
            {
                sb.Append(File.ReadAllText(cssFile));
            }
            sb.Append("</style>");
            // Add Mermaid.js for diagram rendering
            sb.AppendLine("<script src='https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.min.js'></script>");
            sb.AppendLine("<script>mermaid.initialize({ startOnLoad: true, theme: 'default' });</script>");
            sb.AppendLine("</head>");
            sb.AppendLine("<body>");
            sb.AppendLine(Markdown.ToHtml(source, pipeline));
            // Remove title from anchor links so hover shows no redundant tooltip (closer to GitHub)
            sb.AppendLine("<script>(function(){ document.querySelectorAll('a[href^=\"#\"]').forEach(function(a){ a.removeAttribute('title'); }); })();</script>");
            sb.AppendLine("</body>");
            sb.AppendLine("</html>");

            return CreateNativeString(sb.ToString());
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
        Marshal.WriteByte(nativeString, utf8Bytes.Length, 0); // Null terminator
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
