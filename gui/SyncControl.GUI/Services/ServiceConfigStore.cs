using System.IO;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SyncControl.GUI.Services;

public sealed class ServiceConfig
{
    [JsonPropertyName("role")]
    public string Role { get; set; } = "master";

    [JsonPropertyName("bindIp")]
    public string BindIp { get; set; } = "0.0.0.0";

    [JsonPropertyName("port")]
    public int Port { get; set; } = 19881;

    [JsonPropertyName("masterIp")]
    public string MasterIp { get; set; } = string.Empty;

    [JsonPropertyName("masterPort")]
    public int MasterPort { get; set; } = 19881;

    [JsonPropertyName("slavePort")]
    public int SlavePort { get; set; } = 19881;

    [JsonPropertyName("securityKey")]
    public string SecurityKey { get; set; } = string.Empty;

    [JsonPropertyName("autoStart")]
    public bool AutoStart { get; set; }
}

public static class ServiceConfigStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static string FilePath
    {
        get
        {
            var directory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SyncControl");
            Directory.CreateDirectory(directory);
            return Path.Combine(directory, "service-config.json");
        }
    }

    public static ServiceConfig Load()
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                return new ServiceConfig();
            }
            return JsonSerializer.Deserialize<ServiceConfig>(File.ReadAllText(FilePath)) ??
                   new ServiceConfig();
        }
        catch (Exception error)
        {
            FileLogger.Error("load service config failed", error);
            return new ServiceConfig();
        }
    }

    public static void Save(ServiceConfig config)
    {
        try
        {
            // The Core reads the same file when restoring its mode at boot.
            File.WriteAllText(FilePath, JsonSerializer.Serialize(config, JsonOptions),
                new UTF8Encoding(false));
        }
        catch (Exception error)
        {
            FileLogger.Error("save service config failed", error);
        }
    }
}
