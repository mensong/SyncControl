using System.Text.Json.Serialization;
using System.Windows.Media;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SyncControl.GUI.Models;

public enum ConnectionStatus
{
    Offline,
    Connecting,
    Connected,
}

public partial class ScreenNode : ObservableObject
{
    [ObservableProperty]
    [property: JsonPropertyName("id")]
    private string _id = string.Empty;

    [ObservableProperty]
    [property: JsonPropertyName("hostname")]
    private string _hostname = string.Empty;

    [ObservableProperty]
    [property: JsonPropertyName("ip")]
    private string _ip = string.Empty;

    [ObservableProperty]
    [property: JsonPropertyName("screenW")]
    private int _screenW = 1920;

    [ObservableProperty]
    [property: JsonPropertyName("screenH")]
    private int _screenH = 1080;

    [ObservableProperty]
    [property: JsonPropertyName("port")]
    private int _port = 19881;

    [ObservableProperty]
    [property: JsonPropertyName("x")]
    private double _x = 320;

    [ObservableProperty]
    [property: JsonPropertyName("y")]
    private double _y = 180;

    [ObservableProperty]
    [property: JsonPropertyName("width")]
    private double _width = 320;

    [ObservableProperty]
    [property: JsonPropertyName("height")]
    private double _height = 180;

    [ObservableProperty]
    private bool _isMaster;

    [ObservableProperty]
    private ConnectionStatus _connectionStatus = ConnectionStatus.Offline;

    [ObservableProperty]
    private int _latencyMs = -1;

    partial void OnLatencyMsChanged(int value)
    {
        OnPropertyChanged(nameof(LatencyText));
    }

    [JsonIgnore]
    public double Right => X + Width;

    [JsonIgnore]
    public double Bottom => Y + Height;

    [JsonIgnore]
    public Brush StatusBrush => ConnectionStatus switch
    {
        ConnectionStatus.Connected => Brushes.SeaGreen,
        ConnectionStatus.Connecting => Brushes.Goldenrod,
        _ => Brushes.Gray,
    };

    [JsonIgnore]
    public string LatencyText => LatencyMs >= 0 ? $"{LatencyMs} ms" : "";
}
