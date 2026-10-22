using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SyncControl.GUI.Models;
using SyncControl.GUI.Services;
namespace SyncControl.GUI.ViewModels;

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private const double SnapThreshold = 5.0;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    private readonly IpcClient _ipc = new();
    private readonly ServiceManager _serviceManager = new();
    private readonly CancellationTokenSource _monitorCts = new();
    private readonly HashSet<string> _connectedPeerIds = new(StringComparer.OrdinalIgnoreCase);
    private readonly Task _monitorTask;
    private CancellationTokenSource? _layoutUpdateCts;
    private string _lastTransferFileName = string.Empty;
    private bool _settingsOpen;

    [ObservableProperty]
    private string _statusText = "就绪";

    [ObservableProperty]
    private bool _isRunning;

    [ObservableProperty]
    private int _selectedTabIndex;

    partial void OnIsRunningChanged(bool value)
    {
        OnPropertyChanged(nameof(ToggleServiceText));
    }

    [ObservableProperty]
    private string _role = "idle";

    partial void OnSelectedRoleChanged(string value)
    {
        OnPropertyChanged(nameof(IsMasterRole));
        OnPropertyChanged(nameof(IsSlaveRole));
        OnPropertyChanged(nameof(MasterTabEnabled));
        OnPropertyChanged(nameof(SlaveTabEnabled));
        // Follow the role switch when the user sits on the conflicting tab.
        if (value == "master" && SelectedTabIndex == 2)
        {
            SelectedTabIndex = 1;
        }
        else if (value == "slave" && SelectedTabIndex == 1)
        {
            SelectedTabIndex = 2;
        }
    }

    [ObservableProperty]
    private string _clipboardStatusText = "剪贴板：空";

    [ObservableProperty]
    private string _transferProgressText = "文件传输：空闲";

    [ObservableProperty]
    private double _transferProgressPercent;

    [ObservableProperty]
    private bool _isTransferring;

    [ObservableProperty]
    private bool _isServiceError;

    [ObservableProperty]
    private string _selectedRole = "master";

    [ObservableProperty]
    private string _bindIp = "0.0.0.0";

    [ObservableProperty]
    private int _port = 19881;

    [ObservableProperty]
    private string _masterIp = string.Empty;

    [ObservableProperty]
    private int _masterPort = 19881;

    [ObservableProperty]
    private int _slavePort = 19881;

    [ObservableProperty]
    private string _securityKey = string.Empty;

    [ObservableProperty]
    private string _masterStatusText = "等待主控端连接...";

    [ObservableProperty]
    private string _focusText = "焦点：本机";

    [ObservableProperty]
    private string _connectionSummaryText = "已连接 0/0";

    [ObservableProperty]
    private string _peerLatencyText = "";

    [ObservableProperty]
    private bool _autostartEnabled;

    [ObservableProperty]
    private string _networkStatusText = "网络：未知";

    [ObservableProperty]
    private string _mouseLossText = "鼠标丢包率：--";

    [ObservableProperty]
    private bool _manualKcp;

    [ObservableProperty]
    private bool _kcpNodelay = true;

    [ObservableProperty]
    private int _kcpInterval = 10;

    [ObservableProperty]
    private int _kcpResend = 2;

    [ObservableProperty]
    private bool _kcpNoCongestion = true;

    [ObservableProperty]
    private int _kcpSendWindow = 128;

    [ObservableProperty]
    private int _kcpReceiveWindow = 128;

    public event Action<string>? PeerConnected;

    public event Action<string>? PeerDisconnected;

    public event Action<string>? TransferCompleted;

    public ObservableCollection<ScreenNode> Screens { get; } = [];

    public ObservableCollection<EdgeHighlight> Highlights { get; } = [];

    public string LayoutFilePath { get; } = Path.Combine(AppContext.BaseDirectory, "layout.json");

    public bool IsMasterRole
    {
        get => SelectedRole == "master";
        set
        {
            if (value)
            {
                SelectedRole = "master";
            }
        }
    }

    public bool IsSlaveRole
    {
        get => SelectedRole == "slave";
        set
        {
            if (value)
            {
                SelectedRole = "slave";
            }
        }
    }

    public bool MasterTabEnabled => SelectedRole == "master";

    public bool SlaveTabEnabled => SelectedRole == "slave";

    public string ToggleServiceText => IsRunning ? "停止服务" : "启动服务";

    public void SnapToEdges(ScreenNode dragged)
    {
        if (dragged.IsMaster)
        {
            return;
        }

        SnapCoordinate(dragged, true);
        SnapCoordinate(dragged, false);
        RebuildRouting();
    }

    public MainViewModel()
    {
        FileLogger.Info("master GUI started");
        LoadServiceConfig();
        LoadLayout();
        RebuildRouting();

        _ipc.StatusReceived += status => Application.Current?.Dispatcher.Invoke(() => HandleStatus(status));
        _ipc.ConnectionChanged += connected => Application.Current?.Dispatcher.Invoke(() =>
        {
            if (connected)
            {
                IsServiceError = false;
                FileLogger.Info("IPC connection restored");
                return;
            }
            FileLogger.Error("IPC connection lost");
            if (IsRunning)
            {
                IsServiceError = true;
                StatusText = "C++ 服务已崩溃，可重启服务";
            }
            else
            {
                StatusText = "C++ 服务未连接";
            }
        });
        _monitorTask = Task.Run(() => _ipc.PollForeverAsync(_monitorCts.Token));
    }

    [RelayCommand]
    private void EditNode(ScreenNode? node)
    {
        if (node is null)
        {
            return;
        }
        if (ShowNodeEditor(node, isNew: false))
        {
            RebuildRouting();
            SaveLayout();
        }
    }

    [RelayCommand]
    private void DeleteNode(ScreenNode? node)
    {
        if (node is null || node.IsMaster)
        {
            return;
        }
        node.PropertyChanged -= OnScreenPropertyChanged;
        Screens.Remove(node);
        _connectedPeerIds.Remove(node.Id);
        RebuildRouting();
        SaveLayout();
    }

    [RelayCommand]
    private void SaveLayout()
    {
        try
        {
            var config = RoutingBuilder.Build(Screens).Layout;
            File.WriteAllText(LayoutFilePath, JsonSerializer.Serialize(config, JsonOptions), Encoding.UTF8);
            StatusText = "布局已保存";
        }
        catch (Exception error)
        {
            StatusText = $"保存布局失败: {error.Message}";
            FileLogger.Error("save layout failed", error);
        }
    }

    [RelayCommand]
    private async Task ToggleServiceAsync()
    {
        if (IsRunning)
        {
            await StopServiceAsync();
            return;
        }

        await StartServiceAsync();
    }

    private async Task StartServiceAsync()
    {
        PersistServiceConfig();
        StatusText = "正在启动控制...";

        if (!await _serviceManager.StartAsync(SelectedRole))
        {
            StatusText = "无法启动 C++ 服务，请确认 SyncControlCore.exe 存在";
            FileLogger.Error("C++ service failed to start");
            return;
        }

        StatusText = "正在连接 C++ 服务...";
        if (!await _ipc.WaitForReadyAsync())
        {
            StatusText = "启动失败: 无法连接 SyncControl IPC";
            FileLogger.Error("IPC did not become ready after starting the C++ service");
            return;
        }

        JsonObject command;
        if (SelectedRole == "master")
        {
            var layout = RoutingBuilder.Build(Screens).Layout;
            command = new JsonObject
            {
                ["type"] = "start_master",
                ["layout"] = JsonNode.Parse(JsonSerializer.Serialize(layout, JsonOptions)),
                ["bindIp"] = BindIp,
                ["port"] = Port,
                ["securityKey"] = SecurityKey,
            };
        }
        else
        {
            command = new JsonObject
            {
                ["type"] = "start_slave",
                ["localPort"] = SlavePort,
                ["masterIp"] = MasterIp.Trim(),
                ["masterPort"] = MasterPort,
                ["securityKey"] = SecurityKey,
            };
        }

        var response = await _ipc.SendAsync(command);
        if (response.Success)
        {
            IsRunning = true;
            StatusText = SelectedRole == "master" ? "主控模式已启动" : "被控模式已启动";
        }
        else
        {
            StatusText = "启动失败: 无法连接 SyncControl IPC";
        }
    }

    [RelayCommand]
    private async Task StopServiceAsync()
    {
        var response = await _ipc.SendAsync(new JsonObject { ["type"] = "stop" });
        IsRunning = false;
        _connectedPeerIds.Clear();
        foreach (var peer in Screens.Where(node => !node.IsMaster))
        {
            peer.ConnectionStatus = ConnectionStatus.Offline;
        }
        StatusText = response.Success ? "服务已停止" : "停止指令未送达";
    }

    [RelayCommand]
    private async Task RestartServiceAsync()
    {
        StatusText = "正在重启 C++ 服务...";
        FileLogger.Info("restarting C++ service");
        _serviceManager.Stop();
        var resume = IsRunning;
        if (!await _serviceManager.StartAsync(SelectedRole))
        {
            IsServiceError = true;
            StatusText = "重启失败：找不到 C++ 服务程序";
            FileLogger.Error("service restart failed: core executable missing");
            return;
        }
        if (resume)
        {
            await StartServiceAsync();
            return;
        }
        IsServiceError = false;
        StatusText = "服务已重启";
    }

    public void SetAutostart(bool enabled)
    {
        AutostartEnabled = enabled;
        AutostartManager.SetEnabled(enabled, slaveMode: SelectedRole == "slave");
        PersistServiceConfig();
        FileLogger.Info($"autostart {(enabled ? "enabled" : "disabled")}");
    }

    private void LoadServiceConfig()
    {
        var config = ServiceConfigStore.Load();
        SelectedRole = config.Role == "slave" ? "slave" : "master";
        BindIp = string.IsNullOrWhiteSpace(config.BindIp) ? "0.0.0.0" : config.BindIp;
        Port = config.Port > 0 ? config.Port : 19881;
        MasterIp = config.MasterIp;
        MasterPort = config.MasterPort > 0 ? config.MasterPort : 19881;
        SlavePort = config.SlavePort > 0 ? config.SlavePort : 19881;
        SecurityKey = config.SecurityKey;
        AutostartEnabled = AutostartManager.IsEnabled(slaveMode: SelectedRole == "slave");
    }

    private void PersistServiceConfig()
    {
        ServiceConfigStore.Save(new ServiceConfig
        {
            Role = SelectedRole,
            BindIp = BindIp,
            Port = Port,
            MasterIp = MasterIp.Trim(),
            MasterPort = MasterPort,
            SlavePort = SlavePort,
            SecurityKey = SecurityKey,
            AutoStart = AutostartEnabled,
        });
        if (AutostartEnabled)
        {
            // Keep the registered Core arguments in sync with the selected role.
            AutostartManager.SetEnabled(true, slaveMode: SelectedRole == "slave");
        }
    }

    public void Dispose()
    {
        _layoutUpdateCts?.Cancel();
        _monitorCts.Cancel();
        try
        {
            _monitorTask.Wait(200);
        }
        catch (AggregateException)
        {
        }
        _ipc.Dispose();
        _serviceManager.Detach();
        _monitorCts.Dispose();
    }

    private void LoadLayout()
    {
        try
        {
            if (!File.Exists(LayoutFilePath))
            {
                AddMaster();
                return;
            }

            var config = JsonSerializer.Deserialize<LayoutConfig>(File.ReadAllText(LayoutFilePath));
            if (config?.Screens is not { Count: > 0 } screens)
            {
                AddMaster();
                return;
            }

            foreach (var node in screens)
            {
                node.IsMaster = node.Id.Equals("master", StringComparison.OrdinalIgnoreCase);
                AttachScreen(node);
                Screens.Add(node);
            }
            if (!Screens.Any(node => node.IsMaster))
            {
                AddMaster();
            }
        }
        catch (Exception)
        {
            Screens.Clear();
            AddMaster();
        }
    }

    private void AddMaster()
    {
        var master = new ScreenNode
        {
            Id = "master",
            Hostname = Environment.MachineName,
            Ip = "本机",
            ScreenW = (int)SystemParameters.PrimaryScreenWidth,
            ScreenH = (int)SystemParameters.PrimaryScreenHeight,
            X = 320,
            Y = 200,
            IsMaster = true,
            ConnectionStatus = ConnectionStatus.Connected,
        };
        AttachScreen(master);
        Screens.Add(master);
    }

    private void AttachScreen(ScreenNode node)
    {
        node.PropertyChanged += OnScreenPropertyChanged;
    }

    private void OnScreenPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ScreenNode.X) or nameof(ScreenNode.Y) or
            nameof(ScreenNode.Width) or nameof(ScreenNode.Height))
        {
            RebuildRouting();
            if (IsRunning)
            {
                QueueLayoutUpdate();
            }
        }
    }

    private void RebuildRouting()
    {
        var result = RoutingBuilder.Build(Screens);
        Highlights.Clear();
        foreach (var highlight in result.Highlights)
        {
            Highlights.Add(highlight);
        }
    }

    private void SnapCoordinate(ScreenNode dragged, bool vertical)
    {
        var bestDistance = SnapThreshold + 1.0;
        var bestValue = double.NaN;
        foreach (var target in Screens)
        {
            if (ReferenceEquals(target, dragged))
            {
                continue;
            }

            if (vertical)
            {
                var overlaps = Math.Min(dragged.Right, target.Right) - Math.Max(dragged.X, target.X) > 0.5;
                if (!overlaps)
                {
                    continue;
                }
                var candidates = new[]
                {
                    (Math.Abs(dragged.Y - target.Bottom), target.Bottom),
                    (Math.Abs(dragged.Bottom - target.Y), target.Y - dragged.Height),
                };
                var candidate = candidates.MinBy(item => item.Item1);
                if (candidate.Item1 <= SnapThreshold && candidate.Item1 < bestDistance)
                {
                    bestDistance = candidate.Item1;
                    bestValue = candidate.Item2;
                }
            }
            else
            {
                var overlaps = Math.Min(dragged.Bottom, target.Bottom) - Math.Max(dragged.Y, target.Y) > 0.5;
                if (!overlaps)
                {
                    continue;
                }
                var candidates = new[]
                {
                    (Math.Abs(dragged.X - target.Right), target.Right),
                    (Math.Abs(dragged.Right - target.X), target.X - dragged.Width),
                };
                var candidate = candidates.MinBy(item => item.Item1);
                if (candidate.Item1 <= SnapThreshold && candidate.Item1 < bestDistance)
                {
                    bestDistance = candidate.Item1;
                    bestValue = candidate.Item2;
                }
            }
        }

        if (!double.IsNaN(bestValue))
        {
            if (vertical)
            {
                dragged.Y = bestValue;
            }
            else
            {
                dragged.X = bestValue;
            }
        }
    }

    private void QueueLayoutUpdate()
    {
        _layoutUpdateCts?.Cancel();
        _layoutUpdateCts?.Dispose();
        _layoutUpdateCts = new CancellationTokenSource();
        var token = _layoutUpdateCts.Token;
        _ = SendLayoutUpdateAfterDelayAsync(token);
    }

    private async Task SendLayoutUpdateAfterDelayAsync(CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(300, cancellationToken);
            var layout = RoutingBuilder.Build(Screens).Layout;
            var command = new JsonObject
            {
                ["type"] = "update_layout",
                ["layout"] = JsonNode.Parse(JsonSerializer.Serialize(layout, JsonOptions)),
            };
            await _ipc.SendAsync(command, cancellationToken);
        }
        catch (OperationCanceledException)
        {
        }
    }

    private void HandleStatus(JsonObject status)
    {
        var running = status["running"]?.GetValue<bool>() ?? false;
        Role = status["role"]?.GetValue<string>() ?? "idle";
        IsRunning = running;
        var focus = status["focus"]?.GetValue<string>() ?? string.Empty;
        FocusText = string.IsNullOrEmpty(focus) ? "焦点：本机" : $"焦点：{focus}";
        ClipboardStatusText = (status["clipboard"]?["state"]?.GetValue<string>() ?? "empty") switch
        {
            "text" => "剪贴板：文本",
            "files" => "剪贴板：文件",
            _ => "剪贴板：空",
        };
        UpdateTransferProgress(status["transfer"] as JsonObject);
        var current = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var latencyParts = new List<string>();
        var totalPeers = 0;
        var unconfiguredIp = string.Empty;
        if (status["peers"] is JsonArray peers)
        {
            totalPeers = peers.Count;
            foreach (var peerNode in peers.OfType<JsonObject>())
            {
                var id = peerNode["id"]?.GetValue<string>() ??
                         peerNode["endpoint"]?.GetValue<string>() ?? string.Empty;
                if (id.Length == 0)
                {
                    continue;
                }
                var latencyMs = (int)(peerNode["latencyMs"]?.GetValue<double>() ?? -1);
                if (peerNode["connected"]?.GetValue<bool>() != true)
                {
                    SetPeerLatency(id, -1);
                    continue;
                }
                current.Add(id);
                if (_connectedPeerIds.Add(id))
                {
                    SetPeerStatus(id, ConnectionStatus.Connected);
                    PeerConnected?.Invoke(id);
                    FileLogger.Info($"peer connected: {id}");
                }
                if (peerNode["layoutConfigured"]?.GetValue<bool>() == false)
                {
                    var peerIp = peerNode["ip"]?.GetValue<string>() ?? string.Empty;
                    EnsureLayoutNode(id, peerIp);
                    if (unconfiguredIp.Length == 0)
                    {
                        unconfiguredIp = string.IsNullOrEmpty(peerIp) ? id : peerIp;
                    }
                }
                SetPeerLatency(id, latencyMs);
                UpdatePeerResolution(id, peerNode);
                if (latencyMs >= 0)
                {
                    latencyParts.Add($"{id} {latencyMs}ms");
                }
            }
        }

        if (Role == "slave")
        {
            var master = (status["peers"] as JsonArray)?.OfType<JsonObject>().FirstOrDefault();
            MasterStatusText = master is not null && master["connected"]?.GetValue<bool>() == true
                ? $"已连接主控端 {master["ip"]?.GetValue<string>() ?? master["endpoint"]?.GetValue<string>()}"
                : "等待主控端连接...";
        }

        foreach (var disconnectedId in _connectedPeerIds.Except(current).ToArray())
        {
            _connectedPeerIds.Remove(disconnectedId);
            SetPeerStatus(disconnectedId, ConnectionStatus.Offline);
            SetPeerLatency(disconnectedId, -1);
            PeerDisconnected?.Invoke(disconnectedId);
            FileLogger.Info($"peer disconnected: {disconnectedId}");
        }
        ConnectionSummaryText = $"已连接 {current.Count}/{totalPeers}";
        PeerLatencyText = string.Join(" · ", latencyParts);
        StatusText = running ? $"控制运行中 ({Role})" : "服务空闲";
        if (SelectedRole == "master" && unconfiguredIp.Length > 0)
        {
            StatusText = $"{unconfiguredIp}已上线，请进行布局配置";
        }
        UpdateNetworkStatus(status["network"] as JsonObject);
    }

    private void EnsureLayoutNode(string peerId, string peerIp)
    {
        var node = Screens.FirstOrDefault(item => !item.IsMaster && (
            (!string.IsNullOrEmpty(peerIp) &&
             item.Ip.Equals(peerIp, StringComparison.OrdinalIgnoreCase)) ||
            item.Id.Equals(peerId, StringComparison.OrdinalIgnoreCase)));
        if (node is null)
        {
            var master = Screens.FirstOrDefault(item => item.IsMaster);
            node = new ScreenNode
            {
                Id = peerId,
                Hostname = peerId,
                Ip = peerIp,
                ScreenW = master?.ScreenW ?? (int)SystemParameters.PrimaryScreenWidth,
                ScreenH = master?.ScreenH ?? (int)SystemParameters.PrimaryScreenHeight,
                X = (master?.Right ?? 660) + 24,
                Y = master?.Y ?? 200,
            };
            AttachScreen(node);
            Screens.Add(node);
        }
        else
        {
            // Keep the layout id/ip aligned with the live peer so the Core's
            // layoutConfigured check matches after the next update_layout.
            node.Id = peerId;
            if (!string.IsNullOrEmpty(peerIp))
            {
                node.Ip = peerIp;
            }
        }
        RebuildRouting();
        SaveLayout();
    }

    private void UpdateNetworkStatus(JsonObject? network)
    {
        if (network is not { } networkStatus)
        {
            NetworkStatusText = "网络：未知";
            MouseLossText = "鼠标丢包率：--";
            return;
        }

        var networkType = networkStatus["type"]?.GetValue<string>() ?? "unknown";
        var networkName = networkType switch
        {
            "lan" => "LAN",
            "cross" => "跨网段",
            "manual" => "手动配置",
            _ => "未知",
        };
        var kcp = networkStatus["kcp"] as JsonObject;
        NetworkStatusText =
            $"网络：{networkName} · KCP nodelay={KcpFlagText(kcp?["nodelay"])} " +
            $"interval={kcp?["interval"]?.GetValue<int>() ?? 10} " +
            $"resend={kcp?["resend"]?.GetValue<int>() ?? 2} " +
            $"nc={KcpFlagText(kcp?["nc"])} " +
            $"wnd={kcp?["sendWindow"]?.GetValue<int>() ?? 128}/" +
            $"{kcp?["receiveWindow"]?.GetValue<int>() ?? 128}";

        if (networkStatus["mouseLossRate"] is { } lossNode)
        {
            var lost = networkStatus["mouseLost"]?.GetValue<long>() ?? 0;
            MouseLossText = $"鼠标丢包率：{lossNode.GetValue<double>() * 100.0:0.###}% ({lost} 包)";
        }
        else
        {
            MouseLossText = "鼠标丢包率：--";
        }

        if (!_settingsOpen)
        {
            ManualKcp = networkStatus["manualKcp"]?.GetValue<bool>() ?? false;
            KcpNodelay = (kcp?["nodelay"]?.GetValue<int>() ?? 1) != 0;
            KcpInterval = kcp?["interval"]?.GetValue<int>() ?? 10;
            KcpResend = kcp?["resend"]?.GetValue<int>() ?? 2;
            KcpNoCongestion = (kcp?["nc"]?.GetValue<int>() ?? 1) != 0;
            KcpSendWindow = kcp?["sendWindow"]?.GetValue<int>() ?? 128;
            KcpReceiveWindow = kcp?["receiveWindow"]?.GetValue<int>() ?? 128;
        }
    }

    public void SetSettingsOpen(bool isOpen)
    {
        _settingsOpen = isOpen;
    }

    [RelayCommand]
    private async Task ApplyKcpParametersAsync()
    {
        var response = await _ipc.SendAsync(new JsonObject
        {
            ["type"] = "set_kcp_params",
            ["nodelay"] = KcpNodelay ? 1 : 0,
            ["interval"] = KcpInterval,
            ["resend"] = KcpResend,
            ["nc"] = KcpNoCongestion ? 1 : 0,
            ["sendWindow"] = KcpSendWindow,
            ["receiveWindow"] = KcpReceiveWindow,
        });
        StatusText = response.Success ? "KCP 参数已应用" : "KCP 参数无效";
        FileLogger.Info($"manual KCP parameters {(response.Success ? "accepted" : "rejected")}");
    }

    private static string KcpFlagText(JsonNode? value)
    {
        return (value?.GetValue<int>() ?? 0) != 0 ? "1" : "0";
    }

    private void UpdateTransferProgress(JsonObject? transfer)
    {
        if (transfer?["active"]?.GetValue<bool>() == true)
        {
            var fileName = transfer["file"]?.GetValue<string>() ?? "";
            var transferred = transfer["transferred"]?.GetValue<double>() ?? 0;
            var total = transfer["total"]?.GetValue<double>() ?? 0;
            var percent = total > 0 ? Math.Clamp(transferred / total * 100.0, 0.0, 100.0) : 0;
            TransferProgressText = $"传输中: {fileName} {FormatBytes(transferred)} / {FormatBytes(total)} ({percent:0}%)";
            TransferProgressPercent = percent;
            IsTransferring = true;
            _lastTransferFileName = fileName;
            return;
        }

        if (IsTransferring)
        {
            var completed = _lastTransferFileName;
            _lastTransferFileName = string.Empty;
            TransferCompleted?.Invoke(completed);
            FileLogger.Info($"file transfer completed: {completed}");
        }
        TransferProgressText = "文件传输：空闲";
        TransferProgressPercent = 0;
        IsTransferring = false;
    }

    private static string FormatBytes(double bytes)
    {
        const double KiB = 1024;
        const double MiB = KiB * 1024;
        const double GiB = MiB * 1024;
        return bytes switch
        {
            >= GiB => $"{bytes / GiB:0.00} GB",
            >= MiB => $"{bytes / MiB:0.00} MB",
            >= KiB => $"{bytes / KiB:0.0} KB",
            _ => $"{bytes:0} B",
        };
    }

    private void SetPeerStatus(string peerId, ConnectionStatus connectionStatus)
    {
        foreach (var node in Screens)
        {
            if (node.Id.Equals(peerId, StringComparison.OrdinalIgnoreCase) && !node.IsMaster)
            {
                node.ConnectionStatus = connectionStatus;
            }
        }
    }

    private void SetPeerLatency(string peerId, int latencyMs)
    {
        foreach (var node in Screens)
        {
            if (node.Id.Equals(peerId, StringComparison.OrdinalIgnoreCase) && !node.IsMaster)
            {
                node.LatencyMs = latencyMs;
            }
        }
    }

    private bool ShowNodeEditor(ScreenNode node, bool isNew)
    {
        var accepted = false;
        var idBox = new TextBox { Text = node.Id };
        var hostnameBox = new TextBox { Text = node.Hostname };
        var ipBox = new TextBox { Text = node.Ip };
        var portBox = new TextBox { Text = node.Port.ToString() };
        TextBox[] boxes = [idBox, hostnameBox, ipBox, portBox];

        var buttonPanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 16, 0, 0),
        };
        var okButton = new Button { Content = "确定", Width = 76, Margin = new Thickness(0, 0, 8, 0), IsDefault = true };
        var cancelButton = new Button { Content = "取消", Width = 76, IsCancel = true };
        buttonPanel.Children.Add(okButton);
        buttonPanel.Children.Add(cancelButton);

        var panel = new StackPanel { Margin = new Thickness(16) };
        panel.Children.Add(CreateLabel(isNew ? "添加电脑" : "编辑属性"));
        panel.Children.Add(CreateField("ID", idBox));
        panel.Children.Add(CreateField("电脑名称", hostnameBox));
        panel.Children.Add(CreateField("IP", ipBox));
        panel.Children.Add(CreateField("端口", portBox));
        panel.Children.Add(buttonPanel);

        var window = new Window
        {
            Title = isNew ? "添加电脑" : "编辑属性",
            Width = 340,
            SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            ResizeMode = ResizeMode.NoResize,
            Content = panel,
        };
        okButton.Click += (_, _) =>
        {
            if (!int.TryParse(portBox.Text, out var port) ||
                port <= 0 ||
                string.IsNullOrWhiteSpace(idBox.Text))
            {
                MessageBox.Show(window, "请输入有效的 ID 和端口。", "SyncControl");
                return;
            }
            node.Id = idBox.Text.Trim();
            node.Hostname = hostnameBox.Text.Trim();
            node.Ip = ipBox.Text.Trim();
            node.Port = port;
            accepted = true;
            window.Close();
        };
        cancelButton.Click += (_, _) => window.Close();

        var owner = Application.Current?.Windows.OfType<Window>().FirstOrDefault(window => window.IsActive);
        if (owner != null)
        {
            window.Owner = owner;
        }
        window.ShowDialog();
        return accepted;
    }

    private void UpdatePeerResolution(string peerId, JsonObject peerStatus)
    {
        if (peerStatus["connected"]?.GetValue<bool>() != true ||
            peerStatus["screenW"] is not JsonNode widthNode ||
            peerStatus["screenH"] is not JsonNode heightNode)
        {
            return;
        }

        int width;
        int height;
        try
        {
            width = widthNode.GetValue<int>();
            height = heightNode.GetValue<int>();
        }
        catch (FormatException)
        {
            return;
        }

        var node = Screens.FirstOrDefault(item =>
            item.Id.Equals(peerId, StringComparison.OrdinalIgnoreCase) && !item.IsMaster);
        if (node is null || width <= 0 || height <= 0 ||
            (node.ScreenW == width && node.ScreenH == height))
        {
            return;
        }

        node.ScreenW = width;
        node.ScreenH = height;
        if (IsRunning)
        {
            QueueLayoutUpdate();
        }
        SaveLayout();
    }

    private static TextBlock CreateLabel(string text)
    {
        return new TextBlock
        {
            Text = text,
            FontSize = 16,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 0, 0, 12),
        };
    }

    private static Grid CreateField(string header, TextBox textBox)
    {
        var grid = new Grid
        {
            Margin = new Thickness(0, 0, 0, 8),
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(88) },
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
            },
        };
        grid.Children.Add(new TextBlock { Text = header, VerticalAlignment = VerticalAlignment.Center });
        Grid.SetColumn(textBox, 1);
        grid.Children.Add(textBox);
        return grid;
    }
}
