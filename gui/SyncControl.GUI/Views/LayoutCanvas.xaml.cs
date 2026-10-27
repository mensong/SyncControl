using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using SyncControl.GUI.Models;
using SyncControl.GUI.ViewModels;

namespace SyncControl.GUI.Views;

public partial class LayoutCanvas : UserControl
{
    private const double MinScale = 0.25;
    private const double MaxScale = 3.0;
    private const double WheelZoomStep = 1.15;
    private const double PanContextMenuThreshold = 6;

    private ScreenNode? _draggedNode;
    private Point _dragOffset;
    private bool _isPanning;
    private Point _panStartScreen;
    private Point _panStartTranslate;

    public LayoutCanvas()
    {
        InitializeComponent();
    }

    private void Node_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border border || border.DataContext is not ScreenNode node || node.IsMaster)
        {
            return;
        }

        _draggedNode = node;
        _dragOffset = e.GetPosition(border);
        border.CaptureMouse();
        border.Cursor = Cursors.SizeAll;
        e.Handled = true;
    }

    private void Node_MouseMove(object sender, MouseEventArgs e)
    {
        if (_draggedNode is null)
        {
            return;
        }
        if (e.LeftButton != MouseButtonState.Pressed)
        {
            EndDrag(sender);
            return;
        }

        var canvasPoint = e.GetPosition(CanvasHost);
        _draggedNode.X = Math.Max(0, canvasPoint.X - _dragOffset.X);
        _draggedNode.Y = Math.Max(0, canvasPoint.Y - _dragOffset.Y);
        e.Handled = true;
    }

    private void Node_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_draggedNode is null)
        {
            return;
        }

        if (DataContext is MainViewModel viewModel)
        {
            viewModel.SnapToEdges(_draggedNode);
        }
        EndDrag(sender);
        e.Handled = true;
    }

    private void EndDrag(object sender)
    {
        if (sender is FrameworkElement element)
        {
            element.ReleaseMouseCapture();
            element.Cursor = null;
        }
        _draggedNode = null;
    }

    private void CanvasHost_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        var screenPoint = e.GetPosition(RootGrid);
        var canvasPoint = e.GetPosition(CanvasHost);
        var newScale = Math.Clamp(CanvasScale.ScaleX * (e.Delta > 0 ? WheelZoomStep : 1 / WheelZoomStep), MinScale, MaxScale);

        // Keep the canvas point under the cursor fixed while zooming.
        CanvasScale.ScaleX = newScale;
        CanvasScale.ScaleY = newScale;
        CanvasTranslate.X = screenPoint.X - canvasPoint.X * newScale;
        CanvasTranslate.Y = screenPoint.Y - canvasPoint.Y * newScale;
        e.Handled = true;
    }

    private void CanvasHost_PreviewMouseRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        _isPanning = true;
        _panStartScreen = e.GetPosition(RootGrid);
        _panStartTranslate = new Point(CanvasTranslate.X, CanvasTranslate.Y);
        CanvasHost.CaptureMouse();
    }

    private void CanvasHost_MouseMove(object sender, MouseEventArgs e)
    {
        if (!_isPanning)
        {
            return;
        }

        var screenPoint = e.GetPosition(RootGrid);
        CanvasTranslate.X = _panStartTranslate.X + screenPoint.X - _panStartScreen.X;
        CanvasTranslate.Y = _panStartTranslate.Y + screenPoint.Y - _panStartScreen.Y;
    }

    private void CanvasHost_MouseRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (!_isPanning)
        {
            return;
        }

        _isPanning = false;
        CanvasHost.ReleaseMouseCapture();

        var endScreen = e.GetPosition(RootGrid);
        var panDistance = (endScreen - _panStartScreen).Length;
        if (panDistance > PanContextMenuThreshold)
        {
            // Treat a dragged right click as a pan, not a context-menu request.
            e.Handled = true;
        }
    }

    private void ResetViewButton_Click(object sender, RoutedEventArgs e)
    {
        CanvasScale.ScaleX = 1;
        CanvasScale.ScaleY = 1;
        CanvasTranslate.X = 0;
        CanvasTranslate.Y = 0;
    }
}
