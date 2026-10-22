using Microsoft.Win32;

namespace SyncControl.GUI.Services;

public static class AutostartManager
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string AppName = "SyncControl";

    public static bool IsEnabled(bool slaveMode)
    {
        var registered = ReadRegisteredCommand();
        var expected = BuildCommand(slaveMode);
        if (registered == expected)
        {
            return true;
        }

        // Older builds registered the GUI process. Upgrade the entry so the
        // resident Core can restore the last master layout at boot.
        if (!string.IsNullOrEmpty(expected) && IsLegacyGuiCommand(registered, slaveMode))
        {
            SetEnabled(true, slaveMode);
            FileLogger.Info("autostart migrated from GUI to Core");
            return true;
        }

        return false;
    }

    public static void SetEnabled(bool enabled, bool slaveMode)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true);
            if (key is null)
            {
                FileLogger.Error("autostart: cannot open HKCU Run key");
                return;
            }
            if (enabled)
            {
                var command = BuildCommand(slaveMode);
                if (string.IsNullOrEmpty(command))
                {
                    FileLogger.Error("autostart: SyncControlCore.exe not found");
                    return;
                }
                key.SetValue(AppName, command);
            }
            else if (key.GetValue(AppName) is not null)
            {
                key.DeleteValue(AppName);
            }
        }
        catch (Exception error)
        {
            FileLogger.Error("autostart toggle failed", error);
        }
    }

    private static string? ReadRegisteredCommand()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath);
            return key?.GetValue(AppName) as string;
        }
        catch (Exception error)
        {
            FileLogger.Error("autostart read failed", error);
            return null;
        }
    }

    private static string BuildCommand(bool slaveMode)
    {
        // Autostart runs the resident Core, not the on-demand GUI.
        var path = ServiceManager.FindCoreExecutable();
        return string.IsNullOrEmpty(path)
            ? string.Empty
            : slaveMode ? $"\"{path}\" --slave" : $"\"{path}\" --master";
    }

    private static bool IsLegacyGuiCommand(string? command, bool slaveMode)
    {
        var path = Environment.ProcessPath;
        if (string.IsNullOrEmpty(path) || string.IsNullOrEmpty(command))
        {
            return false;
        }

        var legacy = slaveMode ? $"\"{path}\" --slave" : $"\"{path}\"";
        return string.Equals(command, legacy, StringComparison.OrdinalIgnoreCase);
    }
}
