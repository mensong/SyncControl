using System.Diagnostics;
using System.IO;

namespace SyncControl.GUI.Services;

public sealed class ServiceManager : IDisposable
{
    private Process? _process;

    public bool IsRunning => _process is { HasExited: false };

    public async Task<bool> StartAsync(string mode)
    {
        if (IsRunning)
        {
            return true;
        }

        var executable = FindCoreExecutable();
        if (executable is null)
        {
            return false;
        }

        return await Task.Run(() =>
        {
            var existing = FindRunningCore();
            if (existing is not null)
            {
                _process = existing;
                return true;
            }

            var info = new ProcessStartInfo
            {
                FileName = executable,
                Arguments = mode == "slave" ? "--slave" : "--master",
                WorkingDirectory = Path.GetDirectoryName(executable),
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            _process = Process.Start(info);
            if (_process is { } started && started.WaitForExit(200))
            {
                // A race with another launcher is rare, but the singleton core
                // exits immediately in that case. Reuse the winner instead.
                existing = FindRunningCore();
                if (existing is not null)
                {
                    _process = existing;
                    return true;
                }

                _process = null;
                started.Dispose();
                return false;
            }
            return _process != null;
        });
    }

    public void Stop()
    {
        if (_process is { HasExited: false })
        {
            try
            {
                _process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
            }
        }
        _process?.Dispose();
        _process = null;
    }

    public async Task StopAsync(int gracefulTimeoutMs = 3000)
    {
        var process = _process;
        if (process is null || process.HasExited)
        {
            process = FindRunningCore();
            if (process is not null)
            {
                _process = process;
            }
        }

        if (process is { HasExited: false } && gracefulTimeoutMs > 0)
        {
            using var timeout = new CancellationTokenSource(gracefulTimeoutMs);
            try
            {
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
            }
            catch (InvalidOperationException)
            {
            }
        }

        Stop();
    }

    public void Detach()
    {
        // Closing the manager must not stop a shared Core instance. Explicit
        // Stop() remains available for service restarts.
        _process?.Dispose();
        _process = null;
    }

    public void Dispose()
    {
        Detach();
    }

    public static string? FindCoreExecutable()
    {
        // Deployed packages keep the core beside the GUI executable; the
        // repository fallback lets Debug/Release builds work without packaging.
        var deployed = Path.Combine(AppContext.BaseDirectory, "SyncControlCore.exe");
        if (File.Exists(deployed))
        {
            return deployed;
        }

        string? directory = AppContext.BaseDirectory;
        for (var depth = 0; depth < 8 && directory is not null; depth++)
        {
            foreach (var configuration in new[] { "Release", "Debug" })
            {
                var candidate = Path.Combine(directory, "core", "build", configuration, "SyncControlCore.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
            directory = Path.GetDirectoryName(directory);
        }
        return null;
    }

    private static Process? FindRunningCore()
    {
        try
        {
            return Process.GetProcessesByName("SyncControlCore")
                .FirstOrDefault(process => !process.HasExited);
        }
        catch (Exception)
        {
            return null;
        }
    }
}
