using System.Windows;
using System.Threading;
using SyncControl.GUI.Services;
using SyncControl.GUI.Views;

namespace SyncControl.GUI;

public partial class App : Application
{
    private const string InstanceMutexName = @"Local\SyncControl.GUI.Instance";
    private const string ShowEventName = @"Local\SyncControl.GUI.Show";

    private Mutex? _instanceMutex;
    private bool _ownsInstanceMutex;
    private EventWaitHandle? _showRequested;
    private RegisteredWaitHandle? _showWaitRegistration;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _instanceMutex = new Mutex(true, InstanceMutexName, out var isFirstInstance);
        if (!isFirstInstance)
        {
            SignalExistingInstance();
            Shutdown();
            return;
        }
        _ownsInstanceMutex = true;

        _showRequested = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        _showWaitRegistration = ThreadPool.RegisterWaitForSingleObject(
            _showRequested,
            (_, _) => Dispatcher.BeginInvoke(ShowExistingWindow),
            null,
            Timeout.Infinite,
            executeOnlyOnce: false);

        var slave = e.Args.Any(arg => arg.Equals("--slave", StringComparison.OrdinalIgnoreCase));
        FileLogger.Info($"GUI starting ({(slave ? "slave" : "master")})");
        // The GUI is an on-demand manager for both roles; Core stays resident
        // after it closes.
        MainWindow = new MainWindow();
        MainWindow.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _showWaitRegistration?.Unregister(null);
        _showRequested?.Dispose();
        if (_ownsInstanceMutex)
        {
            _instanceMutex?.ReleaseMutex();
        }
        _instanceMutex?.Dispose();
        base.OnExit(e);
    }

    private static void SignalExistingInstance()
    {
        // The event is created just after the mutex, so retry briefly across
        // the tiny startup window between the two operations.
        for (var attempt = 0; attempt < 5; attempt++)
        {
            if (EventWaitHandle.TryOpenExisting(ShowEventName, out var showRequested))
            {
                using (showRequested)
                {
                    showRequested.Set();
                }
                return;
            }
            Thread.Sleep(20);
        }
    }

    private void ShowExistingWindow()
    {
        var window = MainWindow ?? Windows.OfType<Window>().FirstOrDefault();
        if (window is null)
        {
            return;
        }

        window.Show();
        window.WindowState = WindowState.Normal;
        window.Activate();
    }
}
