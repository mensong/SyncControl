using System.Text.Json.Serialization;

namespace SyncControl.GUI.Models;

public enum Edge
{
    Top,
    Bottom,
    Left,
    Right,
}

public class EdgeSegment
{
    [JsonIgnore]
    public Edge FromEdge { get; set; }

    [JsonPropertyName("rangeStart")]
    public float RangeStart { get; set; }

    [JsonPropertyName("rangeEnd")]
    public float RangeEnd { get; set; }

    [JsonPropertyName("toId")]
    public string ToId { get; set; } = string.Empty;

    [JsonPropertyName("toEdge")]
    public Edge ToEdge { get; set; }
}
