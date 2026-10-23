using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Xml.Linq;
using Microsoft.Win32;

namespace SyncControl.GUI.Services;

public static class AutostartManager
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string AppName = "SyncControl";
    private const string TaskName = @"SyncControl\SyncControlCore";
    private const string TaskNamespace = "http://schemas.microsoft.com/windows/2004/02/mit/task";

    public static bool IsEnabled(bool slaveMode)
    {
        if (ReadTaskAction() is { } action && MatchesExpectedAction(action, slaveMode))
        {
            return true;
        }

        // Older builds registered the command in the HKCU Run key. Migrate it
        // to an elevated scheduled task so requireAdministrator binaries can
        // start silently at logon.
        if (ReadRegisteredCommand() is not null)
        {
            RemoveLegacyRunEntry();
            return TryCreateTask(slaveMode);
        }

        return false;
    }

    public static void SetEnabled(bool enabled, bool slaveMode)
    {
        try
        {
            if (enabled)
            {
                if (!TryCreateTask(slaveMode))
                {
                    return;
                }
            }
            else
            {
                DeleteTask();
            }

            RemoveLegacyRunEntry();
        }
        catch (Exception error)
        {
            FileLogger.Error("autostart toggle failed", error);
        }
    }

    private static bool TryCreateTask(bool slaveMode)
    {
        var path = ServiceManager.FindCoreExecutable();
        if (string.IsNullOrEmpty(path))
        {
            FileLogger.Error("autostart: SyncControlCore.exe not found");
            return false;
        }

        var xmlPath = Path.Combine(Path.GetTempPath(), $"SyncControlCore.task.{Guid.NewGuid():N}.xml");
        try
        {
            File.WriteAllText(xmlPath, BuildTaskXml(path, slaveMode), System.Text.Encoding.Unicode);
            if (!RunSchtasks($"/Create /TN \"{TaskName}\" /XML \"{xmlPath}\" /F", out var output))
            {
                FileLogger.Error($"autostart: failed to create scheduled task: {output}");
                return false;
            }

            return true;
        }
        finally
        {
            try
            {
                File.Delete(xmlPath);
            }
            catch
            {
                // A leftover temp file is harmless.
            }
        }
    }

    private static void DeleteTask()
    {
        if (RunSchtasks($"/Query /TN \"{TaskName}\"", out _))
        {
            RunSchtasks($"/Delete /TN \"{TaskName}\" /F", out var output);
            if (output.Length > 0)
            {
                FileLogger.Info($"autostart: deleted scheduled task: {output}");
            }
        }
    }

    private static (string Command, string Arguments)? ReadTaskAction()
    {
        if (!RunSchtasks($"/Query /TN \"{TaskName}\" /XML", out var output) || output.Length == 0)
        {
            return null;
        }

        try
        {
            var document = XDocument.Parse(output);
            var exec = document.Root?
                .Element(XName.Get("Actions", TaskNamespace))?
                .Element(XName.Get("Exec", TaskNamespace));
            var command = exec?.Element(XName.Get("Command", TaskNamespace))?.Value;
            if (string.IsNullOrEmpty(command))
            {
                return null;
            }

            return (command, exec?.Element(XName.Get("Arguments", TaskNamespace))?.Value ?? string.Empty);
        }
        catch (Exception error)
        {
            FileLogger.Error("autostart: failed to read scheduled task", error);
            return null;
        }
    }

    private static bool MatchesExpectedAction((string Command, string Arguments) action, bool slaveMode)
    {
        var path = ServiceManager.FindCoreExecutable();
        if (string.IsNullOrEmpty(path))
        {
            return false;
        }

        var expectedArguments = slaveMode ? "--slave" : "--master";
        return string.Equals(action.Command, path, StringComparison.OrdinalIgnoreCase) &&
               string.Equals(action.Arguments, expectedArguments, StringComparison.OrdinalIgnoreCase);
    }

    private static string BuildTaskXml(string corePath, bool slaveMode)
    {
        var identity = WindowsIdentity.GetCurrent();
        var account = $"{Environment.UserDomainName}\\{Environment.UserName}";
        var arguments = slaveMode ? "--slave" : "--master";

        return $"""
            <?xml version="1.0" encoding="UTF-16"?>
            <Task version="1.2" xmlns="{TaskNamespace}">
              <Triggers>
                <LogonTrigger>
                  <UserId>{account}</UserId>
                </LogonTrigger>
              </Triggers>
              <Principals>
                <Principal id="Author">
                  <UserId>{identity.User?.Value}</UserId>
                  <LogonType>InteractiveToken</LogonType>
                  <RunLevel>Highest</RunLevel>
                </Principal>
              </Principals>
              <Settings>
                <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
                <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
                <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
                <AllowHardTerminate>true</AllowHardTerminate>
                <StartWhenAvailable>true</StartWhenAvailable>
                <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
                <AllowStartOnDemand>true</AllowStartOnDemand>
                <Enabled>true</Enabled>
                <Hidden>false</Hidden>
                <RunOnlyIfIdle>false</RunOnlyIfIdle>
                <WakeToRun>false</WakeToRun>
                <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
                <Priority>7</Priority>
              </Settings>
              <Actions Context="Author">
                <Exec>
                  <Command>{corePath}</Command>
                  <Arguments>{arguments}</Arguments>
                </Exec>
              </Actions>
            </Task>
            """;
    }

    private static bool RunSchtasks(string arguments, out string output)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.SystemDirectory, "schtasks.exe"),
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        using var process = Process.Start(startInfo);
        if (process is null)
        {
            output = "schtasks.exe failed to start";
            return false;
        }

        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();
        output = (stdout + stderr).Trim();
        return process.ExitCode == 0;
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

    private static void RemoveLegacyRunEntry()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true);
            key?.DeleteValue(AppName, throwOnMissingValue: false);
        }
        catch (Exception error)
        {
            FileLogger.Error("autostart legacy cleanup failed", error);
        }
    }
}
