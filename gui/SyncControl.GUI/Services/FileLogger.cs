using System.IO;
using System.Text;

namespace SyncControl.GUI.Services;

// Rotating file log under %LOCALAPPDATA%\SyncControl\gui.log (10 MB max).
public static class FileLogger
{
    private const long MaxLogBytes = 10 * 1024 * 1024;
    private static readonly object Lock = new();
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "SyncControl",
        "gui.log");

    public static void Info(string message) => Write("INFO", message);

    public static void Error(string message, Exception? error = null)
    {
        Write("ERROR", error is null ? message : $"{message}: {error}");
    }

    private static void Write(string level, string message)
    {
        try
        {
            lock (Lock)
            {
                string? directory = Path.GetDirectoryName(LogPath);
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                }
                if (File.Exists(LogPath) && new FileInfo(LogPath).Length > MaxLogBytes)
                {
                    var old = Path.ChangeExtension(LogPath, ".old.log");
                    File.Delete(old);
                    File.Move(LogPath, old);
                }
                File.AppendAllText(
                    LogPath,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{level}] {message}{Environment.NewLine}",
                    Encoding.UTF8);
            }
        }
        catch
        {
            // Logging must never take the app down; there is nowhere left to report.
        }
    }
}
