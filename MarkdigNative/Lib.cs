using System.Runtime.InteropServices;
using System.Text;
using Markdig;

namespace MarkdigNative;

public static class Lib
{
    [UnmanagedCallersOnly(EntryPoint = "ConvertMarkdownToHtml")]
    public static unsafe IntPtr ConvertMarkdownToHtml(IntPtr filenamePtr, IntPtr cssFilePtr, IntPtr extensionsPtr)
    {
        try
        {
            string filename = Marshal.PtrToStringAnsi(filenamePtr) ?? "";
            string cssFile = Marshal.PtrToStringAnsi(cssFilePtr) ?? "";
            string extensions = Marshal.PtrToStringAnsi(extensionsPtr) ?? "";

            string source = File.ReadAllText(filename);
            
            var pipeline = new MarkdownPipelineBuilder()
                .Configure(extensions)
                .Build();

            var sb = new StringBuilder(1000);
            sb.AppendLine("<html><head>");
            sb.AppendLine("<meta charset='utf-8'>");
            sb.AppendLine("<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">");
            
            string baseDir = Path.GetDirectoryName(filename)?.Replace("\\", "/") ?? "";
            sb.AppendLine($"<base href=\"file:///{baseDir}/\"/>");

            sb.Append("<style>");
            if (File.Exists(cssFile))
            {
                sb.Append(File.ReadAllText(cssFile));
            }
            sb.Append("</style>");
            sb.AppendLine("</head>");
            sb.AppendLine("<body>");
            sb.AppendLine(Markdown.ToHtml(source, pipeline));
            sb.AppendLine("</body>");
            sb.AppendLine("</html>");

            string result = sb.ToString();
            byte[] utf8Bytes = Encoding.UTF8.GetBytes(result);
            
            // Allocate memory that can be freed by the caller
            IntPtr nativeString = Marshal.AllocHGlobal(utf8Bytes.Length + 1);
            Marshal.Copy(utf8Bytes, 0, nativeString, utf8Bytes.Length);
            Marshal.WriteByte(nativeString, utf8Bytes.Length, 0); // Null terminator

            return nativeString;
        }
        catch (Exception ex)
        {
            string error = $"<html><body><h1>Error</h1><p>{ex.Message}</p></body></html>";
            byte[] utf8Bytes = Encoding.UTF8.GetBytes(error);
            IntPtr nativeString = Marshal.AllocHGlobal(utf8Bytes.Length + 1);
            Marshal.Copy(utf8Bytes, 0, nativeString, utf8Bytes.Length);
            Marshal.WriteByte(nativeString, utf8Bytes.Length, 0);
            return nativeString;
        }
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
