using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using SyncControl.GUI.Models;
using SyncControl.GUI.ViewModels;

namespace SyncControl.GUI.Views;

public partial class LayoutCanvas : UserControl
{
    private ScreenNode? _draggedNode;
    private Point _dragOffset;

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
}
