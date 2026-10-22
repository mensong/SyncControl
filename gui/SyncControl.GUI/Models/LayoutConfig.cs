using System.Text.Json.Serialization;

namespace SyncControl.GUI.Models;

public class LayoutConfig
{
    [JsonPropertyName("screens")]
    public List<ScreenNode> Screens { get; set; } = [];

    [JsonPropertyName("routing")]
    public Dictionary<string, Dictionary<string, List<EdgeSegment>>> Routing { get; set; } = [];
}
