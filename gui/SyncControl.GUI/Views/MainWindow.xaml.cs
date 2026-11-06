using System.Windows;
using SyncControl.GUI.Services;
using SyncControl.GUI.ViewModels;

namespace SyncControl.GUI.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        FileLogger.Info("main window created");
    }

    private void Autostart_Checked(object sender, RoutedEventArgs e)
    {
        if (DataContext is MainViewModel viewModel)
        {
            viewModel.SetAutostart(true);
        }
    }

    private void Autostart_Unchecked(object sender, RoutedEventArgs e)
    {
        if (DataContext is MainViewModel viewModel)
        {
            viewModel.SetAutostart(false);
        }
    }

    private void Window_Closed(object sender, EventArgs e)
    {
        if (DataContext is MainViewModel viewModel)
        {
            viewModel.Dispose();
        }
    }
}
