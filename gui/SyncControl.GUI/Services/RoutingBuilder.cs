using System.Windows.Media;
using SyncControl.GUI.Models;

namespace SyncControl.GUI.Services;

public sealed class EdgeHighlight
{
    public double X { get; init; }
    public double Y { get; init; }
    public double Width { get; init; }
    public double Height { get; init; }
    public Brush Brush { get; init; } = Brushes.Transparent;
}

public sealed class RoutingResult
{
    public required LayoutConfig Layout { get; init; }
    public required IReadOnlyList<EdgeHighlight> Highlights { get; init; }
}

public static class RoutingBuilder
{
    private const double SnapThreshold = 5.0;
    private const double HighlightThickness = 6.0;

    public static RoutingResult Build(IReadOnlyList<ScreenNode> screens)
    {
        var config = new LayoutConfig
        {
            Screens = screens.Select(Clone).ToList(),
            Routing = [],
        };
        var highlights = new List<EdgeHighlight>();

        foreach (var source in screens)
        {
            var segments = new Dictionary<string, List<EdgeSegment>>(StringComparer.OrdinalIgnoreCase)
            {
                ["top"] = [],
                ["bottom"] = [],
                ["left"] = [],
                ["right"] = [],
            };
            var sourceSegments = new List<EdgeSegment>();

            foreach (var target in screens)
            {
                if (ReferenceEquals(source, target))
                {
                    continue;
                }

                sourceSegments.AddRange(CreateTopSegments(source, target));
                sourceSegments.AddRange(CreateBottomSegments(source, target));
                sourceSegments.AddRange(CreateLeftSegments(source, target));
                sourceSegments.AddRange(CreateRightSegments(source, target));
            }

            foreach (var group in sourceSegments.GroupBy(segment => segment.FromEdge))
            {
                segments[EdgeName(group.Key)] = group
                    .OrderBy(segment => segment.RangeStart)
                    .Select(Normalize)
                    .ToList();
            }

            config.Routing[source.Id] = segments;
            highlights.AddRange(CreateHighlights(source, sourceSegments));
        }

        return new RoutingResult { Layout = config, Highlights = highlights };
    }

    private static IEnumerable<EdgeSegment> CreateTopSegments(ScreenNode source, ScreenNode target)
    {
        if (Math.Abs(source.Y - target.Bottom) > SnapThreshold)
        {
            return [];
        }

        return CreateSegments(source, target, Edge.Top, Edge.Bottom,
            Math.Max(source.X, target.X), Math.Min(source.Right, target.Right), source.X, source.Width);
    }

    private static IEnumerable<EdgeSegment> CreateBottomSegments(ScreenNode source, ScreenNode target)
    {
        if (Math.Abs(source.Bottom - target.Y) > SnapThreshold)
        {
            return [];
        }

        return CreateSegments(source, target, Edge.Bottom, Edge.Top,
            Math.Max(source.X, target.X), Math.Min(source.Right, target.Right), source.X, source.Width);
    }

    private static IEnumerable<EdgeSegment> CreateLeftSegments(ScreenNode source, ScreenNode target)
    {
        if (Math.Abs(source.X - target.Right) > SnapThreshold)
        {
            return [];
        }

        return CreateSegments(source, target, Edge.Left, Edge.Right,
            Math.Max(source.Y, target.Y), Math.Min(source.Bottom, target.Bottom), source.Y, source.Height);
    }

    private static IEnumerable<EdgeSegment> CreateRightSegments(ScreenNode source, ScreenNode target)
    {
        if (Math.Abs(source.Right - target.X) > SnapThreshold)
        {
            return [];
        }

        return CreateSegments(source, target, Edge.Right, Edge.Left,
            Math.Max(source.Y, target.Y), Math.Min(source.Bottom, target.Bottom), source.Y, source.Height);
    }

    private static IEnumerable<EdgeSegment> CreateSegments(ScreenNode source, ScreenNode target,
        Edge fromEdge, Edge toEdge, double overlapStart, double overlapEnd,
        double origin, double length)
    {
        if (overlapEnd - overlapStart <= 0.5 || length <= 0)
        {
            return [];
        }

        return
        [
            new EdgeSegment
            {
                FromEdge = fromEdge,
                RangeStart = NormalizeRatio((overlapStart - origin) / length),
                RangeEnd = NormalizeRatio((overlapEnd - origin) / length),
                ToId = target.Id,
                ToEdge = toEdge,
            },
        ];
    }

    private static IEnumerable<EdgeHighlight> CreateHighlights(ScreenNode source, IEnumerable<EdgeSegment> segments)
    {
        foreach (var segment in segments)
        {
            double x;
            double y;
            double width;
            double height;
            if (segment.FromEdge is Edge.Top or Edge.Bottom)
            {
                y = segment.FromEdge == Edge.Top ? source.Y : source.Bottom;
                x = source.X + (double)segment.RangeStart * source.Width;
                width = (double)(segment.RangeEnd - segment.RangeStart) * source.Width;
                height = HighlightThickness;
                y -= HighlightThickness / 2.0;
            }
            else
            {
                x = segment.FromEdge == Edge.Left ? source.X : source.Right;
                y = source.Y + (double)segment.RangeStart * source.Height;
                height = (double)(segment.RangeEnd - segment.RangeStart) * source.Height;
                width = HighlightThickness;
                x -= HighlightThickness / 2.0;
            }

            yield return new EdgeHighlight
            {
                X = x,
                Y = y,
                Width = Math.Max(2.0, width),
                Height = Math.Max(2.0, height),
                Brush = TargetBrush(segment.ToId),
            };
        }
    }

    public static Brush TargetBrush(string targetId)
    {
        Brush[] palette = [Brushes.MediumSeaGreen, Brushes.SteelBlue, Brushes.MediumOrchid, Brushes.Chocolate];
        var hash = Math.Abs(targetId.GetHashCode(StringComparison.OrdinalIgnoreCase));
        return palette[hash % palette.Length];
    }

    private static EdgeSegment Normalize(EdgeSegment segment)
    {
        segment.RangeStart = Math.Clamp(segment.RangeStart, 0f, 1f);
        segment.RangeEnd = Math.Clamp(segment.RangeEnd, 0f, 1f);
        if (segment.RangeStart > segment.RangeEnd)
        {
            (segment.RangeStart, segment.RangeEnd) = (segment.RangeEnd, segment.RangeStart);
        }
        return segment;
    }

    private static float NormalizeRatio(double value)
    {
        return (float)Math.Round(Math.Clamp(value, 0.0, 1.0), 4);
    }

    private static string EdgeName(Edge edge) => edge switch
    {
        Edge.Top => "top",
        Edge.Bottom => "bottom",
        Edge.Left => "left",
        _ => "right",
    };

    private static ScreenNode Clone(ScreenNode node)
    {
        return new ScreenNode
        {
            Id = node.Id,
            Hostname = node.Hostname,
            Ip = node.Ip,
            ScreenW = node.ScreenW,
            ScreenH = node.ScreenH,
            Port = node.Port,
            X = node.X,
            Y = node.Y,
            Width = node.Width,
            Height = node.Height,
        };
    }
}
