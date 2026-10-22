using System.Windows;
using System.Windows.Controls;
using SyncControl.GUI.Services;
using SyncControl.GUI.ViewModels;

namespace SyncControl.GUI.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        if (DataContext is MainViewModel viewModel)
        {
            // The first tab hosts the KCP editor; keep status pushes from
            // overwriting its fields while it is visible.
            viewModel.SetSettingsOpen(true);
        }
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

    private void MainTabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DataContext is MainViewModel viewModel && sender is TabControl tabs)
        {
            viewModel.SetSettingsOpen(tabs.SelectedIndex == 0);
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
