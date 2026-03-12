using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using System.Windows.Forms.DataVisualization.Charting;

namespace EverVance
{
    public sealed class MainForm : Form
    {
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        private static extern int SetWindowTheme(IntPtr hWnd, string pszSubAppName, string pszSubIdList);

        private readonly Dictionary<uint, CanMessageDefinition> _dbcMap = new Dictionary<uint, CanMessageDefinition>();
        private readonly BindingList<SignalSample> _samples = new BindingList<SignalSample>();
        private readonly BindingList<SignalSample> _storageRows = new BindingList<SignalSample>();
        private readonly BindingList<ChannelConfigRow> _channelConfigs = new BindingList<ChannelConfigRow>();
        private readonly BindingList<TxFrameRow> _txFrames = new BindingList<TxFrameRow>();

        private readonly BindingList<RealtimeVariableRow> _realtimeRows = new BindingList<RealtimeVariableRow>();
        private readonly Dictionary<string, RealtimeVariableRow> _realtimeMap = new Dictionary<string, RealtimeVariableRow>();
        private readonly HashSet<string> _monitoredKeys = new HashSet<string>();
        private readonly List<BusFrameRow> _busRows = new List<BusFrameRow>();
        private readonly BindingList<BusFrameRow> _busViewRows = new BindingList<BusFrameRow>();
        private readonly List<string> _allVariableKeys = new List<string>();

        private ITransport _transport = new MockTransport();
        private readonly Timer _pollTimer = new Timer();
        private readonly Timer _txScheduleTimer = new Timer();
        private readonly Timer _busViewTimer = new Timer();
        private readonly Timer _indicatorTimer = new Timer();
        private readonly Timer _linkHeartbeatTimer = new Timer();
        private bool _txScheduleRunning;
        private bool _projectReady;
        private bool _busViewDirty;
        private bool _projectDirty;
        private bool _suspendDirtyTracking;
        private bool _isConnected;
        private bool _channelConfigSyncPending;
        private bool _rxErrorLatched;
        private bool _txErrorLatched;
        private DateTime _rxFlashUntilUtc = DateTime.MinValue;
        private DateTime _txFlashUntilUtc = DateTime.MinValue;
        private DateTime _lastDeviceActivityUtc = DateTime.MinValue;
        private byte _linkHeartbeatSequence = 0x80;

        private ToolStripStatusLabel _status;
        private ToolStripStatusLabel _ledProject;
        private ToolStripStatusLabel _ledConnect;
        private ToolStripStatusLabel _ledRxFlow;
        private ToolStripStatusLabel _ledRxError;
        private ToolStripStatusLabel _ledTxFlow;
        private ToolStripStatusLabel _ledTxError;
        private ToolStripTextBox _addressBox;
        private ToolStrip _navBar;
        private ToolStripComboBox _endpointPresetBox;
        private ToolStripButton _btnRefreshPorts;
        private ToolStripButton _btnConnect;
        private ToolStripButton _btnDisconnect;
        private ToolStripButton _btnImportDbc;
        private ToolStripButton _btnMonitor;
        private ToolStripButton _btnBus;
        private ToolStripButton _btnStartProject;
        private ToolStripButton _btnEndProject;
        private ToolStripButton _btnThemeToggle;
        private Button _btnAddChannel;
        private Button _btnRemoveChannel;
        private ComboBox _channelAddBox;
        private DataGridView _channelGrid;
        private ComboBox _txFrameChannelAddBox;
        private DataGridView _txFrameGrid;
        private TreeView _dbcTree;
        private Chart _chart;
        private DataGridView _grid;
        private TabControl _tabs;
        private Panel _tabHeaderPanel;
        private readonly List<Button> _tabHeaderButtons = new List<Button>();
        private SplitContainer _mainSplit;
        private Label _workspacePathLabel;
        private ListBox _workspaceProjectList;
        private ToolTip _workspacePathTip = new ToolTip();
        private string _workspacePath;
        private SplitContainer _plotSplit;

        private ListBox _availableVarList;
        private CheckedListBox _monitoredVarList;
        private ComboBox _timestampFormatBox;
        private DataGridView _realtimeGrid;
        private DataGridView _busGrid;
        private TextBox _busSearchBox;
        private ComboBox _busSearchFieldBox;
        private CheckBox _busOnlyErrorBox;
        private ComboBox _busSortBox;
        private CheckedListBox _busChannelFilter;
        private TextBox _findVariableBox;
        private ComboBox _findVariableFieldBox;
        private TextBox _storageSearchBox;
        private ComboBox _storageSearchFieldBox;
        private string _currentProjectPath;
        private string _currentDbcPath;
        private string _busLogPath;
        private string _appStatePath;
        private Dictionary<string, string> _appState = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        private readonly Timer _startupAnimTimer = new Timer();
        private float _startupOpacity = 0.92f;
        private Icon _appIcon;
        private Panel _workspaceWatermarkCanvas;

        private bool _darkTheme = false;
        private Color UiBg = Color.FromArgb(20, 22, 27);
        private Color UiPanel = Color.FromArgb(30, 33, 40);
        private Color UiPanelAlt = Color.FromArgb(36, 40, 48);
        private Color UiBorder = Color.FromArgb(68, 73, 86);
        private Color UiText = Color.FromArgb(232, 236, 242);
        private Color UiMuted = Color.FromArgb(152, 160, 175);
        private Color UiAccent = Color.FromArgb(0, 215, 210);
        private Color UiAccentHover = Color.FromArgb(0, 240, 230);

        private const byte PacketSync = 0xA5;
        private const byte PacketFlagCanFd = 0x01;
        private const byte PacketFlagTx = 0x02;
        private const byte PacketFlagError = 0x04;
        private const byte PacketFlagControl = 0x08;

        private const byte ProtocolVersion = 1;
        private const byte ProtocolCmdSetChannelConfig = 0x01;
        private const byte ProtocolCmdGetChannelConfig = 0x02;
        private const byte ProtocolCmdGetDeviceInfo = 0x03;
        private const byte ProtocolCmdGetChannelCapabilities = 0x04;
        private const byte ProtocolCmdGetRuntimeStatus = 0x05;
        private const byte ProtocolCmdHeartbeat = 0x06;
        private const byte ProtocolStatusOk = 0x00;
        private const byte ProtocolStatusInvalid = 0x01;
        private const byte ProtocolStatusStagedOnly = 0x02;
        private const byte ProtocolStatusInternalError = 0x03;

        private const string SamplePresetCustom = "自定义";
        private const string FrameTypeClassic = "CAN";
        private const string FrameTypeFd = "CAN FD";
        private static readonly string[] NominalSamplePresetItems = { "75.0%", "80.0%", "87.5%", SamplePresetCustom };
        private static readonly string[] DataSamplePresetItems = { "60.0%", "70.0%", "75.0%", "80.0%", SamplePresetCustom };
        private static readonly byte[] LinkHeartbeatPayload = { 0x4C, 0x49, 0x4E, 0x4B };
        private static readonly byte[] UnlinkHeartbeatPayload = { 0x55, 0x4E, 0x4C, 0x4B };

        public MainForm()
        {
            Text = "EverVance";
            Width = 1680;
            Height = 980;
            MinimumSize = new Size(1360, 860);
            StartPosition = FormStartPosition.CenterScreen;
            KeyPreview = true;
            AutoScaleMode = AutoScaleMode.Dpi;
            ShowIcon = true;
            _appStatePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "EverVance", "appstate.ini");
            SetThemePalette(false);
            ApplyEverVanceIcon();

            BuildUi();

            _pollTimer.Interval = 30;
            _pollTimer.Tick += delegate { PollTransport(); };
            _pollTimer.Start();

            _txScheduleTimer.Interval = 10;
            _txScheduleTimer.Tick += delegate { OnTxScheduleTick(); };
            _busViewTimer.Interval = 500;
            _busViewTimer.Tick += delegate
            {
                if (_busViewDirty)
                {
                    _busViewDirty = false;
                    RefreshBusMonitorView();
                }
            };
            _busViewTimer.Start();
            _indicatorTimer.Interval = 120;
            _indicatorTimer.Tick += delegate { RenderIndicators(); };
            _indicatorTimer.Start();
            _linkHeartbeatTimer.Interval = 1000;
            _linkHeartbeatTimer.Tick += delegate { SendBackgroundHeartbeat(); };
            KeyDown += MainForm_KeyDown;
            FormClosing += MainForm_FormClosing;
        }

        private sealed class RealtimeVariableRow
        {
            public string Variable { get; set; }
            public string Channel { get; set; }
            public string Value { get; set; }
            public string Timestamp { get; set; }
        }

        private sealed class BusFrameRow
        {
            public DateTime SortTime { get; set; }
            public string Timestamp { get; set; }
            public int Channel { get; set; }
            public string Direction { get; set; }
            public string FrameType { get; set; }
            public string IdHex { get; set; }
            public int Dlc { get; set; }
            public string DataHex { get; set; }
            public string ErrorState { get; set; }
            public string ErrorType { get; set; }
        }

        private sealed class ChannelConfigRow
        {
            // 通道编号，限定 0~3
            public int ChannelId { get; set; }
            // 是否打开该通道参与收发
            public bool Enabled { get; set; }
            // 帧类型：CAN / CAN FD
            public string FrameType { get; set; }
            // 仲裁域波特率
            public int NominalBitrate { get; set; }
            // 仲裁域采样点预设
            public string NominalSamplePreset { get; set; }
            // 仲裁域自定义采样点，字符串格式便于 DataGridView 直接编辑
            public string NominalSamplePointText { get; set; }
            // 数据域波特率（仅 CAN FD 有效）
            public int DataBitrate { get; set; }
            // 数据域采样点预设（仅 CAN FD 有效）
            public string DataSamplePreset { get; set; }
            // 数据域自定义采样点（仅 CAN FD 有效）
            public string DataSamplePointText { get; set; }
            // 是否接入板端终端电阻
            public bool TerminationEnabled { get; set; }
        }

        private sealed class TxFrameRow
        {
            // 是否启用该发送帧
            public bool Enabled { get; set; }
            // 目标通道号
            public int ChannelId { get; set; }
            // 帧ID（十六进制字符串）
            public string IdHex { get; set; }
            // 数据区（十六进制字节，空格分隔）
            public string DataHex { get; set; }
            // 定时发送间隔，单位 ms；<=0 表示不参与定时发送
            public int IntervalMs { get; set; }
            // 备注
            public string Note { get; set; }
            // 记录上次发送时间，避免重复触发
            public DateTime LastSentUtc { get; set; }
        }

        private void BuildUi()
        {
            SuspendLayout();

            var menu = new MenuStrip();
            var file = new ToolStripMenuItem("文件");
            file.DropDownItems.Add("新建工程", null, delegate { NewProject(); });
            file.DropDownItems.Add("打开工程", null, delegate { OpenProject(); });
            file.DropDownItems.Add("保存工程", null, delegate { SaveProject(); });
            file.DropDownItems.Add("另存工程", null, delegate { SaveProjectAs(); });
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("导入DBC", null, delegate { ImportDbc(); });
            file.DropDownItems.Add("导出变量CSV", null, delegate { ExportCsv(); });
            file.DropDownItems.Add("退出", null, delegate { Close(); });
            menu.Items.Add(file);

            var help = new ToolStripMenuItem("帮助");
            help.DropDownItems.Add("快速开始（Host_PC/README.md）", null, delegate { OpenDocumentationEntry("Host_PC/README.md"); });
            help.DropDownItems.Add("EverVance 说明（Host_PC/EverVance/README.md）", null, delegate { OpenDocumentationEntry("Host_PC/EverVance/README.md"); });
            help.DropDownItems.Add("文档目录（Host_PC/docs）", null, delegate { OpenDocumentationEntry("Host_PC/docs"); });
            menu.Items.Add(help);

            var about = new ToolStripMenuItem("关于");
            about.DropDownItems.Add("关于 EverVance", null, delegate { ShowAboutDialog(); });
            menu.Items.Add(about);
            menu.Dock = DockStyle.Top;

            var nav = new ToolStrip();
            _navBar = nav;
            nav.Items.Add(new ToolStripLabel("地址栏"));
            _addressBox = new ToolStripTextBox();
            _addressBox.Width = 280;
            _addressBox.Text = "mock://localhost";
            nav.Items.Add(_addressBox);
            nav.Items.Add(new ToolStripLabel("预设"));
            _endpointPresetBox = new ToolStripComboBox();
            _endpointPresetBox.Width = 300;
            _endpointPresetBox.DropDownWidth = 560;
            _endpointPresetBox.DropDownStyle = ComboBoxStyle.DropDownList;
            _endpointPresetBox.SelectedIndexChanged += delegate { ApplyEndpointPresetSelection(); };
            nav.Items.Add(_endpointPresetBox);
            var btnRefreshPorts = new ToolStripButton("刷新预设");
            _btnRefreshPorts = btnRefreshPorts;
            btnRefreshPorts.Click += delegate { RefreshEndpointPresets(true); };
            nav.Items.Add(btnRefreshPorts);

            nav.Items.Add(new ToolStripSeparator());
            var btnStartProject = new ToolStripButton("开始工程");
            _btnStartProject = btnStartProject;
            btnStartProject.Image = CreateStartIcon();
            btnStartProject.ImageTransparentColor = Color.Magenta;
            btnStartProject.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnStartProject.Click += delegate { StartSelectedWorkspaceProject(); };
            nav.Items.Add(btnStartProject);
            var btnEndProject = new ToolStripButton("结束工程");
            _btnEndProject = btnEndProject;
            btnEndProject.Image = CreateStopIcon();
            btnEndProject.ImageTransparentColor = Color.Magenta;
            btnEndProject.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnEndProject.Click += delegate { EndCurrentProject(); };
            nav.Items.Add(btnEndProject);

            var btnConnect = new ToolStripButton("连接");
            _btnConnect = btnConnect;
            btnConnect.Image = CreateLinkIcon();
            btnConnect.ImageTransparentColor = Color.Magenta;
            btnConnect.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnConnect.Click += delegate { ConnectTransport(); };
            nav.Items.Add(btnConnect);

            var btnDisconnect = new ToolStripButton("断开");
            _btnDisconnect = btnDisconnect;
            btnDisconnect.Image = CreateUnlinkIcon();
            btnDisconnect.ImageTransparentColor = Color.Magenta;
            btnDisconnect.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnDisconnect.Click += delegate { DisconnectTransport(); };
            nav.Items.Add(btnDisconnect);

            nav.Items.Add(new ToolStripSeparator());
            var btnImportDbc = new ToolStripButton("导入DBC");
            _btnImportDbc = btnImportDbc;
            btnImportDbc.Image = CreateDocIcon();
            btnImportDbc.ImageTransparentColor = Color.Magenta;
            btnImportDbc.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnImportDbc.Click += delegate { ImportDbc(); };
            nav.Items.Add(btnImportDbc);

            var btnMonitor = new ToolStripButton("变量监控");
            _btnMonitor = btnMonitor;
            btnMonitor.Image = CreateChartIcon();
            btnMonitor.ImageTransparentColor = Color.Magenta;
            btnMonitor.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnMonitor.Click += delegate
            {
                if (_tabs != null)
                {
                    _tabs.SelectedIndex = 2;
                }
            };
            nav.Items.Add(btnMonitor);

            var btnBus = new ToolStripButton("总线监控");
            _btnBus = btnBus;
            btnBus.Image = CreateBusIcon();
            btnBus.ImageTransparentColor = Color.Magenta;
            btnBus.TextImageRelation = TextImageRelation.ImageBeforeText;
            btnBus.Click += delegate
            {
                if (_tabs != null)
                {
                    _tabs.SelectedIndex = 3;
                }
            };
            nav.Items.Add(btnBus);

            nav.Items.Add(new ToolStripSeparator());
            var btnThemeToggle = new ToolStripButton("浅色主题");
            _btnThemeToggle = btnThemeToggle;
            btnThemeToggle.Click += delegate { ToggleTheme(); };
            nav.Items.Add(btnThemeToggle);
            nav.Dock = DockStyle.Top;

            _tabs = new ThemedTabControl();
            _tabs.Dock = DockStyle.Fill;
            _tabs.Margin = new Padding(0);
            _tabs.Padding = new Point(0, 0);
            _tabs.HandleCreated += delegate
            {
                try
                {
                    SetWindowTheme(_tabs.Handle, "", "");
                }
                catch
                {
                }
            };
            var themedTabs = _tabs as ThemedTabControl;
            if (themedTabs != null)
            {
                themedTabs.Owner = this;
            }
            _tabs.Appearance = TabAppearance.FlatButtons;
            _tabs.SizeMode = TabSizeMode.Fixed;
            _tabs.ItemSize = new Size(1, 1);
            _tabs.Multiline = true;
            _tabs.Alignment = TabAlignment.Top;
            _tabs.SelectedIndexChanged += delegate { UpdateTabHeaderButtons(); };
            _tabs.TabPages.Add(BuildCanSendTab());
            _tabs.TabPages.Add(BuildDbcTab());
            _tabs.TabPages.Add(BuildPlotTab());
            _tabs.TabPages.Add(BuildBusMonitorTab());
            _tabs.TabPages.Add(BuildStorageTab());

            _mainSplit = new SplitContainer();
            _mainSplit.Dock = DockStyle.Fill;
            _mainSplit.SplitterDistance = 380;
            _mainSplit.FixedPanel = FixedPanel.Panel1;
            _mainSplit.IsSplitterFixed = false;
            _mainSplit.Panel1MinSize = 300;
            _mainSplit.Panel1.Controls.Add(BuildWorkspacePanel());
            _tabHeaderPanel = BuildTabHeaderPanel();
            var tabsHost = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                Margin = new Padding(0),
                Padding = new Padding(0),
                BackColor = UiBg,
                ColumnCount = 1,
                RowCount = 2
            };
            tabsHost.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
            tabsHost.RowStyles.Add(new RowStyle(SizeType.Absolute, 44f));
            tabsHost.RowStyles.Add(new RowStyle(SizeType.Percent, 100f));
            tabsHost.Controls.Add(_tabHeaderPanel, 0, 0);
            tabsHost.Controls.Add(_tabs, 0, 1);
            _mainSplit.Panel2.Controls.Add(tabsHost);

            var statusBar = new StatusStrip();
            _status = new ToolStripStatusLabel("未连接");
            _status.Spring = true;
            _status.TextAlign = ContentAlignment.MiddleLeft;
            statusBar.Items.Add(_status);
            statusBar.Items.Add(new ToolStripStatusLabel("工程"));
            _ledProject = new ToolStripStatusLabel("●");
            _ledProject.ToolTipText = "工程打开状态";
            statusBar.Items.Add(_ledProject);
            statusBar.Items.Add(new ToolStripStatusLabel("连接"));
            _ledConnect = new ToolStripStatusLabel("●");
            _ledConnect.ToolTipText = "设备连接状态";
            statusBar.Items.Add(_ledConnect);
            statusBar.Items.Add(new ToolStripStatusLabel("RX"));
            _ledRxFlow = new ToolStripStatusLabel("●");
            _ledRxFlow.ToolTipText = "RX 数据流状态（绿灯闪烁）";
            statusBar.Items.Add(_ledRxFlow);
            _ledRxError = new ToolStripStatusLabel("●");
            _ledRxError.ToolTipText = "RX 错误状态（红灯常亮）";
            statusBar.Items.Add(_ledRxError);
            statusBar.Items.Add(new ToolStripStatusLabel("TX"));
            _ledTxFlow = new ToolStripStatusLabel("●");
            _ledTxFlow.ToolTipText = "TX 数据流状态（绿灯闪烁）";
            statusBar.Items.Add(_ledTxFlow);
            _ledTxError = new ToolStripStatusLabel("●");
            _ledTxError.ToolTipText = "TX 错误状态（红灯常亮）";
            statusBar.Items.Add(_ledTxError);
            statusBar.Dock = DockStyle.Bottom;

            Controls.Add(_mainSplit);
            Controls.Add(nav);
            Controls.Add(menu);
            Controls.Add(statusBar);

            MainMenuStrip = menu;
            ResumeLayout();
            SetProjectReady(false);
            LoadAppState();
            ApplyLayoutFromAppState();
            SetWorkspacePath(LoadLastWorkspacePath());
            if (!PromptWorkspaceOnStartup())
            {
                BeginInvoke(new Action(delegate { Close(); }));
                return;
            }
            RefreshWorkspaceProjects();
            RefreshEndpointPresets(false);
            SetStatus("请选择 WorkSpace 下的工程并点击“开始工程”，或先新建工程。");
            WireProjectDirtyTracking();
            UpdateWindowTitle();
            ApplyGeekTheme();
            StartUiAnimation();
            UpdateActionStates();
            RenderIndicators();
            UpdateTabHeaderButtons();
        }

        private TabPage BuildCanSendTab()
        {
            var page = new TabPage("CAN发送与参数");
            var root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.ColumnCount = 1;
            root.RowCount = 2;
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 46));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 54));

            var channelGroup = new GroupBox();
            channelGroup.Text = "通道管理（添加 / 打开关闭 / 配置）";
            channelGroup.Dock = DockStyle.Fill;

            var channelLayout = new TableLayoutPanel();
            channelLayout.Dock = DockStyle.Fill;
            channelLayout.ColumnCount = 1;
            channelLayout.RowCount = 2;
            channelLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            channelLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var channelTop = new Panel();
            channelTop.Dock = DockStyle.Fill;

            channelTop.Controls.Add(new Label { Text = "添加通道", Left = 8, Top = 12, Width = 60 });
            _channelAddBox = new ComboBox();
            _channelAddBox.Left = 74;
            _channelAddBox.Top = 8;
            _channelAddBox.Width = 80;
            _channelAddBox.DropDownStyle = ComboBoxStyle.DropDownList;
            _channelAddBox.Items.AddRange(new object[] { "0", "1", "2", "3" });
            _channelAddBox.SelectedIndex = 0;
            channelTop.Controls.Add(_channelAddBox);

            var addChannelBtn = new Button();
            _btnAddChannel = addChannelBtn;
            addChannelBtn.Text = "添加";
            addChannelBtn.Left = 164;
            addChannelBtn.Top = 7;
            addChannelBtn.Width = 80;
            addChannelBtn.Height = 28;
            addChannelBtn.Click += delegate { AddChannelConfig(); };
            channelTop.Controls.Add(addChannelBtn);

            var removeChannelBtn = new Button();
            _btnRemoveChannel = removeChannelBtn;
            removeChannelBtn.Text = "删除选中";
            removeChannelBtn.Left = 252;
            removeChannelBtn.Top = 7;
            removeChannelBtn.Width = 100;
            removeChannelBtn.Height = 28;
            removeChannelBtn.Click += delegate { RemoveSelectedChannelConfig(); };
            channelTop.Controls.Add(removeChannelBtn);

            var channelHint = new Label();
            channelHint.Text = "CAN 只配置仲裁域采样点；CAN FD 额外开放数据域波特率与采样点。参数会在每次连接成功后自动同步到设备。";
            channelHint.Left = 360;
            channelHint.Top = 12;
            channelHint.Width = 900;
            channelHint.ForeColor = Color.DimGray;
            channelTop.Controls.Add(channelHint);

            _channelGrid = new DataGridView();
            _channelGrid.Dock = DockStyle.Fill;
            _channelGrid.AutoGenerateColumns = false;
            _channelGrid.AllowUserToAddRows = false;
            _channelGrid.AllowUserToDeleteRows = false;
            _channelGrid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
            _channelGrid.MultiSelect = false;

            var colId = new DataGridViewTextBoxColumn();
            colId.DataPropertyName = "ChannelId";
            colId.HeaderText = "通道";
            colId.ReadOnly = true;
            colId.Width = 70;
            _channelGrid.Columns.Add(colId);

            var colEnable = new DataGridViewCheckBoxColumn();
            colEnable.DataPropertyName = "Enabled";
            colEnable.HeaderText = "打开";
            colEnable.Width = 60;
            _channelGrid.Columns.Add(colEnable);

            var colType = new DataGridViewComboBoxColumn();
            colType.DataPropertyName = "FrameType";
            colType.HeaderText = "帧格式";
            colType.Items.AddRange(new object[] { FrameTypeClassic, FrameTypeFd });
            colType.Width = 90;
            _channelGrid.Columns.Add(colType);

            var colNom = new DataGridViewTextBoxColumn();
            colNom.DataPropertyName = "NominalBitrate";
            colNom.HeaderText = "N波特率";
            colNom.Width = 110;
            _channelGrid.Columns.Add(colNom);

            var colNomPreset = new DataGridViewComboBoxColumn();
            colNomPreset.DataPropertyName = "NominalSamplePreset";
            colNomPreset.HeaderText = "N采样点";
            colNomPreset.Items.AddRange(NominalSamplePresetItems);
            colNomPreset.Width = 92;
            _channelGrid.Columns.Add(colNomPreset);

            var colNomCustom = new DataGridViewTextBoxColumn();
            colNomCustom.DataPropertyName = "NominalSamplePointText";
            colNomCustom.HeaderText = "N自定义%";
            colNomCustom.Width = 84;
            _channelGrid.Columns.Add(colNomCustom);

            var colData = new DataGridViewTextBoxColumn();
            colData.DataPropertyName = "DataBitrate";
            colData.HeaderText = "D波特率";
            colData.Width = 110;
            _channelGrid.Columns.Add(colData);

            var colDataPreset = new DataGridViewComboBoxColumn();
            colDataPreset.DataPropertyName = "DataSamplePreset";
            colDataPreset.HeaderText = "D采样点";
            colDataPreset.Items.AddRange(DataSamplePresetItems);
            colDataPreset.Width = 92;
            _channelGrid.Columns.Add(colDataPreset);

            var colDataCustom = new DataGridViewTextBoxColumn();
            colDataCustom.DataPropertyName = "DataSamplePointText";
            colDataCustom.HeaderText = "D自定义%";
            colDataCustom.Width = 84;
            _channelGrid.Columns.Add(colDataCustom);

            var colTerm = new DataGridViewCheckBoxColumn();
            colTerm.DataPropertyName = "TerminationEnabled";
            colTerm.HeaderText = "终端";
            colTerm.Width = 60;
            _channelGrid.Columns.Add(colTerm);

            _channelGrid.DataSource = _channelConfigs;
            _channelGrid.CellFormatting += ChannelGridCellFormatting;
            _channelGrid.CellBeginEdit += ChannelGridCellBeginEdit;
            _channelGrid.CellEndEdit += ChannelGridCellEndEdit;
            _channelGrid.CellValueChanged += ChannelGridCellValueChanged;
            _channelGrid.DataError += delegate { };
            _channelGrid.CurrentCellDirtyStateChanged += delegate
            {
                if (_channelGrid.IsCurrentCellDirty && ShouldCommitChannelGridCurrentCellImmediately())
                {
                    _channelGrid.CommitEdit(DataGridViewDataErrorContexts.Commit);
                }
            };

            channelLayout.Controls.Add(channelTop, 0, 0);
            channelLayout.Controls.Add(_channelGrid, 0, 1);
            channelGroup.Controls.Add(channelLayout);

            var sendGroup = new GroupBox();
            sendGroup.Text = "发送帧列表（可添加多帧）";
            sendGroup.Dock = DockStyle.Fill;

            var sendLayout = new TableLayoutPanel();
            sendLayout.Dock = DockStyle.Fill;
            sendLayout.ColumnCount = 1;
            sendLayout.RowCount = 3;
            sendLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            sendLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            sendLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));

            var sendTop = new Panel { Dock = DockStyle.Fill };
            sendTop.Controls.Add(new Label { Text = "新增帧通道", Left = 8, Top = 12, Width = 68 });
            _txFrameChannelAddBox = new ComboBox();
            _txFrameChannelAddBox.Left = 82;
            _txFrameChannelAddBox.Top = 8;
            _txFrameChannelAddBox.Width = 80;
            _txFrameChannelAddBox.DropDownStyle = ComboBoxStyle.DropDownList;
            _txFrameChannelAddBox.Items.AddRange(new object[] { "0", "1", "2", "3" });
            _txFrameChannelAddBox.SelectedIndex = 0;
            sendTop.Controls.Add(_txFrameChannelAddBox);

            var addFrameBtn = new Button { Text = "添加帧", Left = 170, Top = 7, Width = 80, Height = 28 };
            addFrameBtn.Click += delegate { AddTxFrameRow(); };
            sendTop.Controls.Add(addFrameBtn);

            var removeFrameBtn = new Button { Text = "删除选中帧", Left = 258, Top = 7, Width = 100, Height = 28 };
            removeFrameBtn.Click += delegate { RemoveSelectedTxFrameRow(); };
            sendTop.Controls.Add(removeFrameBtn);

            var sendHint = new Label();
            sendHint.Text = "同一通道帧格式由上方统一决定；Alt+1..9 可单次发送第1..9条启用帧。";
            sendHint.Left = 370;
            sendHint.Top = 12;
            sendHint.Width = 800;
            sendHint.ForeColor = Color.DimGray;
            sendTop.Controls.Add(sendHint);

            _txFrameGrid = new DataGridView();
            _txFrameGrid.Dock = DockStyle.Fill;
            _txFrameGrid.AutoGenerateColumns = false;
            _txFrameGrid.AllowUserToAddRows = false;
            _txFrameGrid.AllowUserToDeleteRows = false;
            _txFrameGrid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
            _txFrameGrid.MultiSelect = false;

            var txEnable = new DataGridViewCheckBoxColumn { DataPropertyName = "Enabled", HeaderText = "启用", Width = 60 };
            _txFrameGrid.Columns.Add(txEnable);
            var txCh = new DataGridViewComboBoxColumn { DataPropertyName = "ChannelId", HeaderText = "通道", Width = 70 };
            ((DataGridViewComboBoxColumn)txCh).Items.AddRange(new object[] { 0, 1, 2, 3 });
            _txFrameGrid.Columns.Add(txCh);
            var txId = new DataGridViewTextBoxColumn { DataPropertyName = "IdHex", HeaderText = "ID(HEX)", Width = 120 };
            _txFrameGrid.Columns.Add(txId);
            var txData = new DataGridViewTextBoxColumn { DataPropertyName = "DataHex", HeaderText = "Data(HEX)", Width = 420 };
            _txFrameGrid.Columns.Add(txData);
            var txInterval = new DataGridViewTextBoxColumn { DataPropertyName = "IntervalMs", HeaderText = "间隔(ms)", Width = 90 };
            _txFrameGrid.Columns.Add(txInterval);
            var txNote = new DataGridViewTextBoxColumn { DataPropertyName = "Note", HeaderText = "备注", Width = 260 };
            _txFrameGrid.Columns.Add(txNote);
            _txFrameGrid.DataSource = _txFrames;

            var sendBottom = new Panel { Dock = DockStyle.Fill };
            var sendSelectedBtn = new Button { Text = "发送选中帧", Left = 8, Top = 7, Width = 120, Height = 28 };
            sendSelectedBtn.Click += delegate { SendSelectedTxFrame(); };
            sendBottom.Controls.Add(sendSelectedBtn);
            var sendAllBtn = new Button { Text = "发送全部启用帧", Left = 138, Top = 7, Width = 130, Height = 28 };
            sendAllBtn.Click += delegate { SendAllEnabledTxFrames(); };
            sendBottom.Controls.Add(sendAllBtn);
            var startCycleBtn = new Button { Text = "开始定时发送", Left = 278, Top = 7, Width = 120, Height = 28 };
            startCycleBtn.Click += delegate { StartTxSchedule(); };
            sendBottom.Controls.Add(startCycleBtn);
            var stopCycleBtn = new Button { Text = "停止定时发送", Left = 408, Top = 7, Width = 120, Height = 28 };
            stopCycleBtn.Click += delegate { StopTxSchedule(); };
            sendBottom.Controls.Add(stopCycleBtn);

            sendLayout.Controls.Add(sendTop, 0, 0);
            sendLayout.Controls.Add(_txFrameGrid, 0, 1);
            sendLayout.Controls.Add(sendBottom, 0, 2);
            sendGroup.Controls.Add(sendLayout);

            root.Controls.Add(channelGroup, 0, 0);
            root.Controls.Add(sendGroup, 0, 1);
            page.Controls.Add(root);

            AddChannelConfig(0);
            AddChannelConfig(1);
            AddTxFrameRow(0);
            return page;
        }

        private void SetThemePalette(bool dark)
        {
            _darkTheme = dark;
            if (dark)
            {
                UiBg = Color.FromArgb(20, 22, 27);
                UiPanel = Color.FromArgb(30, 33, 40);
                UiPanelAlt = Color.FromArgb(36, 40, 48);
                UiBorder = Color.FromArgb(68, 73, 86);
                UiText = Color.FromArgb(232, 236, 242);
                UiMuted = Color.FromArgb(152, 160, 175);
                UiAccent = Color.FromArgb(0, 215, 210);
                UiAccentHover = Color.FromArgb(0, 240, 230);
            }
            else
            {
                UiBg = Color.FromArgb(244, 247, 252);
                UiPanel = Color.FromArgb(234, 239, 246);
                UiPanelAlt = Color.FromArgb(223, 230, 239);
                UiBorder = Color.FromArgb(173, 184, 199);
                UiText = Color.FromArgb(24, 31, 42);
                UiMuted = Color.FromArgb(89, 104, 124);
                UiAccent = Color.FromArgb(0, 124, 200);
                UiAccentHover = Color.FromArgb(0, 156, 235);
            }

            if (_btnThemeToggle != null)
            {
                _btnThemeToggle.Text = _darkTheme ? "浅色主题" : "深色主题";
            }
        }

        private void ToggleTheme()
        {
            SetThemePalette(!_darkTheme);
            ApplyGeekTheme();
            Refresh();
        }

        private void ApplyGeekTheme()
        {
            SetThemePalette(_darkTheme);
            BackColor = UiBg;
            ForeColor = UiText;
            Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Regular, GraphicsUnit.Point);
            Padding = new Padding(0);
            Margin = new Padding(0);

            foreach (Control c in Controls)
            {
                ApplyGeekThemeRecursive(c);
            }

            ApplyToolbarSemanticStyle();

            if (_tabs != null)
            {
                _tabs.DrawMode = TabDrawMode.Normal;
                _tabs.ItemSize = new Size(1, 1);
                _tabs.SizeMode = TabSizeMode.Fixed;
                _tabs.Appearance = TabAppearance.FlatButtons;
                _tabs.Multiline = true;
                _tabs.Alignment = TabAlignment.Top;
                _tabs.Padding = new Point(0, 0);
                _tabs.BackColor = UiBg;
                _tabs.ForeColor = UiText;
                _tabs.Margin = new Padding(0);
                try
                {
                    if (_tabs.IsHandleCreated)
                    {
                        SetWindowTheme(_tabs.Handle, "", "");
                    }
                }
                catch
                {
                }
            }

            foreach (var strip in Controls.OfType<ToolStrip>())
            {
                ApplyToolStripStyle(strip);
            }
            foreach (var menu in Controls.OfType<MenuStrip>())
            {
                ApplyToolStripStyle(menu);
            }
            foreach (var status in Controls.OfType<StatusStrip>())
            {
                ApplyToolStripStyle(status);
            }

            if (_mainSplit != null)
            {
                _mainSplit.BackColor = UiBorder;
                _mainSplit.Panel1.BackColor = UiPanel;
                _mainSplit.Panel2.BackColor = UiBg;
                _mainSplit.Panel1.Padding = new Padding(0);
                _mainSplit.Panel2.Padding = new Padding(0);
            }
            if (_plotSplit != null)
            {
                _plotSplit.BackColor = UiBorder;
                _plotSplit.Panel1.BackColor = UiPanel;
                _plotSplit.Panel2.BackColor = UiPanelAlt;
            }
            if (_workspaceWatermarkCanvas != null)
            {
                _workspaceWatermarkCanvas.Invalidate();
            }

            if (_addressBox != null)
            {
                _addressBox.BackColor = _darkTheme ? Color.FromArgb(15, 20, 29) : Color.FromArgb(249, 251, 255);
                _addressBox.ForeColor = UiText;
                _addressBox.BorderStyle = BorderStyle.FixedSingle;
            }
            if (_endpointPresetBox != null)
            {
                _endpointPresetBox.BackColor = _darkTheme ? Color.FromArgb(15, 20, 29) : Color.FromArgb(249, 251, 255);
                _endpointPresetBox.ForeColor = UiText;
                _endpointPresetBox.ComboBox.BackColor = _darkTheme ? Color.FromArgb(15, 20, 29) : Color.FromArgb(249, 251, 255);
                _endpointPresetBox.ComboBox.ForeColor = UiText;
                _endpointPresetBox.ComboBox.FlatStyle = FlatStyle.Flat;
            }
            if (_tabHeaderPanel != null)
            {
                _tabHeaderPanel.BackColor = UiPanel;
            }
            UpdateTabHeaderButtons();
        }

        private Panel BuildTabHeaderPanel()
        {
            var panel = new Panel
            {
                Dock = DockStyle.Fill,
                Height = 44,
                BackColor = UiPanel,
                Padding = new Padding(8, 6, 8, 6),
                Margin = new Padding(0)
            };
            var flow = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                WrapContents = false,
                FlowDirection = FlowDirection.LeftToRight,
                BackColor = UiPanel,
                Margin = new Padding(0),
                Padding = new Padding(0)
            };
            panel.Controls.Add(flow);
            _tabHeaderButtons.Clear();

            var names = new[] { "CAN发送与参数", "DBC与变量", "变量监控", "总线监控", "变量存储" };
            for (int i = 0; i < names.Length; i++)
            {
                var idx = i;
                var btn = new Button
                {
                    Text = names[i],
                    Width = GetTabHeaderButtonWidth(names[i]),
                    Height = 30,
                    Margin = new Padding(0, 0, 8, 0),
                    Tag = idx
                };
                btn.Click += delegate
                {
                    if (_tabs != null && idx >= 0 && idx < _tabs.TabPages.Count)
                    {
                        _tabs.SelectedIndex = idx;
                    }
                };
                flow.Controls.Add(btn);
                _tabHeaderButtons.Add(btn);
            }
            return panel;
        }

        private int GetTabHeaderButtonWidth(string text)
        {
            var size = TextRenderer.MeasureText(text ?? "", new Font("Microsoft YaHei UI", 9f, FontStyle.Regular, GraphicsUnit.Point));
            return Math.Max(118, size.Width + 28);
        }

        private void UpdateTabHeaderButtons()
        {
            if (_tabs == null || _tabHeaderButtons == null || _tabHeaderButtons.Count == 0)
            {
                return;
            }
            for (int i = 0; i < _tabHeaderButtons.Count; i++)
            {
                var btn = _tabHeaderButtons[i];
                bool active = i == _tabs.SelectedIndex;
                btn.BackColor = active
                    ? (_darkTheme ? Color.FromArgb(0, 95, 105) : Color.FromArgb(196, 225, 246))
                    : (_darkTheme ? Color.FromArgb(34, 40, 52) : Color.FromArgb(216, 225, 236));
                btn.ForeColor = active ? UiAccentHover : UiText;
                btn.FlatStyle = FlatStyle.Flat;
                btn.FlatAppearance.BorderSize = 1;
                btn.FlatAppearance.BorderColor = active ? UiAccent : UiBorder;
                btn.FlatAppearance.MouseOverBackColor = btn.BackColor;
                btn.FlatAppearance.MouseDownBackColor = btn.BackColor;
            }
        }

        private bool IsTabHeaderButton(Button btn)
        {
            return btn != null && _tabHeaderButtons != null && _tabHeaderButtons.Contains(btn);
        }

        private void ApplyToolbarSemanticStyle()
        {
            if (_btnStartProject != null) _btnStartProject.ForeColor = Color.FromArgb(80, 220, 130);
            if (_btnEndProject != null) _btnEndProject.ForeColor = Color.FromArgb(245, 110, 110);
            if (_btnConnect != null) _btnConnect.ForeColor = Color.FromArgb(95, 180, 255);
            if (_btnDisconnect != null) _btnDisconnect.ForeColor = Color.FromArgb(255, 170, 95);
            if (_btnImportDbc != null) _btnImportDbc.ForeColor = UiAccent;
            if (_btnMonitor != null) _btnMonitor.ForeColor = UiAccent;
            if (_btnBus != null) _btnBus.ForeColor = UiAccent;
            if (_btnThemeToggle != null) _btnThemeToggle.ForeColor = UiAccentHover;
        }

        private void StyleBandPanel(Panel panel)
        {
            if (panel == null)
            {
                return;
            }
            panel.Tag = "band";
            panel.Padding = new Padding(6, 4, 6, 4);
            panel.Paint -= BandPanel_Paint;
            panel.Paint += BandPanel_Paint;
        }

        private void BandPanel_Paint(object sender, PaintEventArgs e)
        {
            var panel = sender as Panel;
            if (panel == null)
            {
                return;
            }

            var rect = new Rectangle(0, 0, Math.Max(1, panel.Width - 1), Math.Max(1, panel.Height - 1));
            using (var bg = new SolidBrush(UiPanelAlt))
            using (var pen = new Pen(UiBorder))
            {
                e.Graphics.FillRectangle(bg, rect);
                e.Graphics.DrawRectangle(pen, rect);
            }
        }

        private void ApplyGeekThemeRecursive(Control ctrl)
        {
            if (ctrl == null)
            {
                return;
            }

            ctrl.ForeColor = UiText;
            ctrl.Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Regular, GraphicsUnit.Point);
            if (ctrl is TabPage)
            {
                ctrl.BackColor = UiBg;
                ctrl.Padding = new Padding(0);
                var tp = (TabPage)ctrl;
                tp.UseVisualStyleBackColor = false;
            }
            else if (ctrl is GroupBox || ctrl is Panel || ctrl is TableLayoutPanel || ctrl is SplitContainer || ctrl is UserControl)
            {
                if (ctrl is Panel && string.Equals(Convert.ToString(ctrl.Tag), "band", StringComparison.OrdinalIgnoreCase))
                {
                    ctrl.BackColor = UiPanelAlt;
                }
                else
                {
                    ctrl.BackColor = ctrl.Parent is TabPage ? UiBg : UiPanel;
                }
            }

            var btn = ctrl as Button;
            if (btn != null)
            {
                btn.FlatStyle = FlatStyle.Flat;
                btn.FlatAppearance.BorderColor = UiBorder;
                btn.FlatAppearance.BorderSize = 1;
                btn.BackColor = _darkTheme ? Color.FromArgb(34, 40, 52) : Color.FromArgb(216, 225, 236);
                btn.ForeColor = UiAccent;
                btn.Padding = new Padding(8, 2, 8, 2);
                btn.FlatAppearance.MouseOverBackColor = Color.Empty;
                btn.FlatAppearance.MouseDownBackColor = Color.Empty;
                ApplyRoundedRegion(btn, 8);
                AttachButtonHoverAnimation(btn);
            }

            var txt = ctrl as TextBox;
            if (txt != null)
            {
                txt.BorderStyle = BorderStyle.FixedSingle;
                txt.BackColor = _darkTheme ? Color.FromArgb(16, 20, 29) : Color.FromArgb(248, 250, 253);
                txt.ForeColor = UiText;
            }

            var cmb = ctrl as ComboBox;
            if (cmb != null)
            {
                cmb.FlatStyle = FlatStyle.Flat;
                cmb.BackColor = _darkTheme ? Color.FromArgb(18, 24, 35) : Color.FromArgb(248, 250, 253);
                cmb.ForeColor = UiText;
            }

            var chk = ctrl as CheckBox;
            if (chk != null)
            {
                chk.FlatStyle = FlatStyle.Flat;
                chk.BackColor = UiPanel;
                chk.ForeColor = UiText;
            }

            var gb = ctrl as GroupBox;
            if (gb != null)
            {
                gb.BackColor = UiPanelAlt;
                gb.ForeColor = UiMuted;
                gb.Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Bold, GraphicsUnit.Point);
                gb.Padding = new Padding(8);
                gb.FlatStyle = FlatStyle.Flat;
                gb.Paint -= GroupBox_Paint;
                gb.Paint += GroupBox_Paint;
            }

            var list = ctrl as ListBox;
            if (list != null)
            {
                list.BorderStyle = BorderStyle.FixedSingle;
                list.BackColor = _darkTheme ? Color.FromArgb(14, 18, 27) : Color.FromArgb(246, 248, 252);
                list.ForeColor = UiText;
            }

            var checkedList = ctrl as CheckedListBox;
            if (checkedList != null)
            {
                checkedList.BorderStyle = BorderStyle.FixedSingle;
                checkedList.BackColor = _darkTheme ? Color.FromArgb(14, 18, 27) : Color.FromArgb(246, 248, 252);
                checkedList.ForeColor = UiText;
            }

            var tree = ctrl as TreeView;
            if (tree != null)
            {
                tree.BorderStyle = BorderStyle.FixedSingle;
                tree.BackColor = _darkTheme ? Color.FromArgb(14, 18, 27) : Color.FromArgb(246, 248, 252);
                tree.ForeColor = UiText;
                tree.LineColor = UiBorder;
            }

            var grid = ctrl as DataGridView;
            if (grid != null)
            {
                grid.BackgroundColor = UiBg;
                grid.BorderStyle = BorderStyle.None;
                grid.EnableHeadersVisualStyles = false;
                grid.GridColor = UiBorder;
                grid.DefaultCellStyle.BackColor = UiBg;
                grid.DefaultCellStyle.ForeColor = UiText;
                grid.DefaultCellStyle.SelectionBackColor = Color.FromArgb(0, 85, 110);
                grid.DefaultCellStyle.SelectionForeColor = UiText;
                grid.ColumnHeadersDefaultCellStyle.BackColor = UiPanelAlt;
                grid.ColumnHeadersDefaultCellStyle.ForeColor = UiAccent;
                grid.ColumnHeadersDefaultCellStyle.SelectionBackColor = UiPanelAlt;
                grid.ColumnHeadersDefaultCellStyle.SelectionForeColor = UiAccent;
                grid.RowHeadersDefaultCellStyle.BackColor = UiPanelAlt;
                grid.RowHeadersDefaultCellStyle.ForeColor = UiMuted;
                grid.AlternatingRowsDefaultCellStyle.BackColor = Color.FromArgb(25, 28, 34);
                if (!_darkTheme)
                {
                    grid.AlternatingRowsDefaultCellStyle.BackColor = Color.FromArgb(236, 242, 250);
                    grid.DefaultCellStyle.SelectionBackColor = Color.FromArgb(205, 228, 248);
                }
            }

            var chart = ctrl as Chart;
            if (chart != null)
            {
                chart.BackColor = UiBg;
                if (chart.ChartAreas.Count > 0)
                {
                    foreach (var area in chart.ChartAreas)
                    {
                        area.BackColor = UiBg;
                        area.AxisX.LineColor = UiBorder;
                        area.AxisY.LineColor = UiBorder;
                        area.AxisX.LabelStyle.ForeColor = UiMuted;
                        area.AxisY.LabelStyle.ForeColor = UiMuted;
                        area.AxisX.MajorGrid.LineColor = Color.FromArgb(40, 45, 55);
                        area.AxisY.MajorGrid.LineColor = Color.FromArgb(40, 45, 55);
                    }
                }
                foreach (var legend in chart.Legends)
                {
                    legend.BackColor = UiPanel;
                    legend.ForeColor = UiText;
                }
            }

            foreach (Control child in ctrl.Controls)
            {
                ApplyGeekThemeRecursive(child);
            }
        }

        private void ApplyToolStripStyle(ToolStrip strip)
        {
            if (strip == null)
            {
                return;
            }
            strip.BackColor = UiPanel;
            strip.ForeColor = UiText;
            strip.GripStyle = ToolStripGripStyle.Hidden;
            strip.Padding = new Padding(0);
            strip.Margin = new Padding(0);
            strip.RenderMode = ToolStripRenderMode.Professional;
            strip.Renderer = new GeekToolStripRenderer(new GeekColorTable(
                UiPanel,
                UiAccent,
                UiBorder,
                _darkTheme ? Color.FromArgb(0, 80, 95) : Color.FromArgb(198, 224, 246),
                _darkTheme ? Color.FromArgb(0, 72, 82) : Color.FromArgb(186, 214, 238)));
            foreach (ToolStripItem item in strip.Items)
            {
                item.ForeColor = UiText;
            }
        }

        private void StartUiAnimation()
        {
            Opacity = _startupOpacity;
            _startupAnimTimer.Stop();
            _startupAnimTimer.Interval = 18;
            _startupAnimTimer.Tick -= StartupAnimTimer_Tick;
            _startupAnimTimer.Tick += StartupAnimTimer_Tick;
            _startupAnimTimer.Start();
        }

        private void StartupAnimTimer_Tick(object sender, EventArgs e)
        {
            _startupOpacity += 0.03f;
            if (_startupOpacity >= 1f)
            {
                _startupOpacity = 1f;
                Opacity = 1f;
                _startupAnimTimer.Stop();
                return;
            }
            Opacity = _startupOpacity;
        }

        private void AttachButtonHoverAnimation(Button btn)
        {
            btn.MouseEnter -= Button_MouseEnter;
            btn.MouseLeave -= Button_MouseLeave;
            btn.MouseEnter += Button_MouseEnter;
            btn.MouseLeave += Button_MouseLeave;
        }

        private void Button_MouseEnter(object sender, EventArgs e)
        {
            var btn = sender as Button;
            if (btn == null) return;
            if (IsTabHeaderButton(btn))
            {
                var isActive = _tabs != null && btn.Tag is int && (int)btn.Tag == _tabs.SelectedIndex;
                btn.BackColor = isActive
                    ? (_darkTheme ? Color.FromArgb(0, 110, 120) : Color.FromArgb(182, 217, 244))
                    : (_darkTheme ? Color.FromArgb(44, 48, 58) : Color.FromArgb(205, 224, 242));
                btn.ForeColor = UiAccentHover;
                btn.FlatAppearance.BorderColor = UiAccent;
                return;
            }

            btn.BackColor = _darkTheme ? Color.FromArgb(44, 48, 58) : Color.FromArgb(205, 224, 242);
            btn.ForeColor = UiAccentHover;
            btn.FlatAppearance.BorderColor = UiAccent;
        }

        private void Button_MouseLeave(object sender, EventArgs e)
        {
            var btn = sender as Button;
            if (btn == null) return;
            if (IsTabHeaderButton(btn))
            {
                UpdateTabHeaderButtons();
                return;
            }
            btn.BackColor = _darkTheme ? Color.FromArgb(34, 40, 52) : Color.FromArgb(216, 225, 236);
            btn.ForeColor = UiAccent;
            btn.FlatAppearance.BorderColor = UiBorder;
        }

        private static void ApplyRoundedRegion(Control ctrl, int radius)
        {
            if (ctrl == null || radius <= 0)
            {
                return;
            }

            Action update = delegate
            {
                var rect = new Rectangle(0, 0, Math.Max(1, ctrl.Width), Math.Max(1, ctrl.Height));
                using (var path = CreateRoundRectPath(rect, radius))
                {
                    ctrl.Region = new Region(path);
                }
            };
            update();
            ctrl.Resize += delegate { update(); };
        }

        private static GraphicsPath CreateRoundRectPath(Rectangle rect, int radius)
        {
            var path = new GraphicsPath();
            int d = radius * 2;
            path.StartFigure();
            path.AddArc(rect.X, rect.Y, d, d, 180, 90);
            path.AddArc(rect.Right - d, rect.Y, d, d, 270, 90);
            path.AddArc(rect.Right - d, rect.Bottom - d, d, d, 0, 90);
            path.AddArc(rect.X, rect.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }

        private void Tabs_DrawItem(object sender, DrawItemEventArgs e)
        {
            if (_tabs == null || e.Index < 0 || e.Index >= _tabs.TabPages.Count)
            {
                return;
            }
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var rect = e.Bounds;
            bool selected = (e.State & DrawItemState.Selected) == DrawItemState.Selected;
            using (var bg = new SolidBrush(selected ? (_darkTheme ? Color.FromArgb(0, 95, 105) : Color.FromArgb(196, 225, 246)) : UiPanel))
            using (var pen = new Pen(selected ? UiAccent : UiBorder))
            {
                var r = new Rectangle(rect.X + 2, rect.Y + 2, rect.Width - 4, rect.Height - 4);
                using (var path = CreateRoundRectPath(r, 7))
                {
                    g.FillPath(bg, path);
                    g.DrawPath(pen, path);
                }
            }
            TextRenderer.DrawText(
                g,
                _tabs.TabPages[e.Index].Text,
                Font,
                rect,
                selected ? UiAccentHover : UiText,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }

        private void Tabs_Paint(object sender, PaintEventArgs e)
        {
            if (_tabs == null)
            {
                return;
            }

            var g = e.Graphics;
            var tabStripHeight = 26;
            if (_tabs.TabCount > 0)
            {
                try
                {
                    int maxBottom = 0;
                    for (int i = 0; i < _tabs.TabCount; i++)
                    {
                        var r = _tabs.GetTabRect(i);
                        if (r.Bottom > maxBottom)
                        {
                            maxBottom = r.Bottom;
                        }
                    }
                    tabStripHeight = Math.Max(26, maxBottom + 2);
                }
                catch
                {
                    tabStripHeight = Math.Max(26, _tabs.ItemSize.Height + 2);
                }
            }
            using (var bg = new SolidBrush(UiPanel))
            using (var pen = new Pen(UiBorder))
            {
                g.FillRectangle(bg, new Rectangle(0, 0, _tabs.Width, tabStripHeight));
                g.DrawLine(pen, 0, tabStripHeight - 1, _tabs.Width, tabStripHeight - 1);
            }
        }

        private void GroupBox_Paint(object sender, PaintEventArgs e)
        {
            var gb = sender as GroupBox;
            if (gb == null)
            {
                return;
            }

            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            e.Graphics.Clear(gb.BackColor);
            var textSize = TextRenderer.MeasureText(gb.Text, gb.Font);
            var textRect = new Rectangle(10, 0, textSize.Width + 8, textSize.Height);
            using (var textBg = new SolidBrush(gb.BackColor))
            using (var pen = new Pen(UiBorder))
            {
                var borderRect = new Rectangle(1, textRect.Height / 2, gb.Width - 3, gb.Height - textRect.Height / 2 - 2);
                e.Graphics.DrawRectangle(pen, borderRect);
                e.Graphics.FillRectangle(textBg, textRect);
            }
            TextRenderer.DrawText(e.Graphics, gb.Text, gb.Font, new Point(14, 0), UiMuted);
        }

        private sealed class GeekToolStripRenderer : ToolStripProfessionalRenderer
        {
            public GeekToolStripRenderer(ProfessionalColorTable table)
                : base(table)
            {
            }
        }

        private sealed class ThemedTabControl : TabControl
        {
            public MainForm Owner { get; set; }

            protected override void OnPaintBackground(PaintEventArgs e)
            {
                PaintTabBackground(e.Graphics);
            }

            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);
                PaintTabHeaderGap(e.Graphics);
            }

            private void PaintTabBackground(Graphics g)
            {
                if (Width <= 0 || Height <= 0 || g == null)
                {
                    return;
                }

                var owner = Owner;
                var headerColor = owner == null ? BackColor : owner.UiPanel;
                var borderColor = owner == null ? Color.DimGray : owner.UiBorder;
                var contentColor = owner == null ? BackColor : owner.UiBg;
                var stripHeight = Math.Max(26, DisplayRectangle.Top);
                using (var headerBrush = new SolidBrush(headerColor))
                using (var contentBrush = new SolidBrush(contentColor))
                using (var pen = new Pen(borderColor))
                {
                    g.FillRectangle(contentBrush, ClientRectangle);
                    g.FillRectangle(headerBrush, new Rectangle(0, 0, Width, stripHeight));
                    g.DrawLine(pen, 0, stripHeight - 1, Width, stripHeight - 1);
                }
            }

            private void PaintTabHeaderGap(Graphics g)
            {
                if (Width <= 0 || Height <= 0 || g == null)
                {
                    return;
                }

                var owner = Owner;
                var headerColor = owner == null ? BackColor : owner.UiPanel;
                var borderColor = owner == null ? Color.DimGray : owner.UiBorder;
                var headerBottom = Math.Max(1, DisplayRectangle.Top);
                int firstLeft = Width;
                int lastRight = 0;
                for (int i = 0; i < TabCount; i++)
                {
                    var r = GetTabRect(i);
                    if (r.Left < firstLeft) firstLeft = r.Left;
                    if (r.Right > lastRight) lastRight = r.Right;
                }
                if (TabCount == 0)
                {
                    firstLeft = 0;
                    lastRight = 0;
                }

                using (var brush = new SolidBrush(headerColor))
                using (var pen = new Pen(borderColor))
                {
                    if (firstLeft > 0)
                    {
                        g.FillRectangle(brush, new Rectangle(0, 0, firstLeft, headerBottom));
                    }
                    if (lastRight < Width)
                    {
                        g.FillRectangle(brush, new Rectangle(Math.Max(0, lastRight), 0, Width - Math.Max(0, lastRight), headerBottom));
                    }
                    g.DrawLine(pen, 0, headerBottom - 1, Width, headerBottom - 1);
                }
            }
        }

        private sealed class GeekColorTable : ProfessionalColorTable
        {
            private readonly Color _panel;
            private readonly Color _accent;
            private readonly Color _border;
            private readonly Color _hover;
            private readonly Color _press;

            public GeekColorTable(Color panel, Color accent, Color border, Color hover, Color press)
            {
                _panel = panel;
                _accent = accent;
                _border = border;
                _hover = hover;
                _press = press;
            }

            public override Color ToolStripGradientBegin { get { return _panel; } }
            public override Color ToolStripGradientMiddle { get { return _panel; } }
            public override Color ToolStripGradientEnd { get { return _panel; } }
            public override Color MenuStripGradientBegin { get { return _panel; } }
            public override Color MenuStripGradientEnd { get { return _panel; } }
            public override Color StatusStripGradientBegin { get { return _panel; } }
            public override Color StatusStripGradientEnd { get { return _panel; } }
            public override Color ImageMarginGradientBegin { get { return _panel; } }
            public override Color ImageMarginGradientMiddle { get { return _panel; } }
            public override Color ImageMarginGradientEnd { get { return _panel; } }
            public override Color ButtonSelectedHighlight { get { return _hover; } }
            public override Color ButtonSelectedBorder { get { return _accent; } }
            public override Color ButtonPressedGradientBegin { get { return _press; } }
            public override Color ButtonPressedGradientMiddle { get { return _press; } }
            public override Color ButtonPressedGradientEnd { get { return _press; } }
            public override Color MenuItemSelected { get { return _hover; } }
            public override Color MenuItemSelectedGradientBegin { get { return _hover; } }
            public override Color MenuItemSelectedGradientEnd { get { return _hover; } }
            public override Color MenuItemPressedGradientBegin { get { return _press; } }
            public override Color MenuItemPressedGradientEnd { get { return _press; } }
            public override Color SeparatorDark { get { return _border; } }
            public override Color SeparatorLight { get { return _border; } }
        }

        private TabPage BuildDbcTab()
        {
            var page = new TabPage("DBC与变量");
            _dbcTree = new TreeView();
            _dbcTree.Dock = DockStyle.Fill;
            page.Controls.Add(_dbcTree);
            return page;
        }

        private TabPage BuildPlotTab()
        {
            var page = new TabPage("变量监控");
            var split = new SplitContainer();
            _plotSplit = split;
            split.Dock = DockStyle.Fill;
            split.SplitterDistance = 460;
            split.IsSplitterFixed = false;

            var left = new TableLayoutPanel();
            left.Dock = DockStyle.Fill;
            left.ColumnCount = 1;
            left.RowCount = 8;
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 30));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 25));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 45));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 8));

            var topPanel = new Panel { Dock = DockStyle.Fill };
            StyleBandPanel(topPanel);
            var backBtn = new Button { Text = "返回发送页", Width = 110, Height = 28, Left = 6, Top = 5 };
            backBtn.Click += delegate { _tabs.SelectedIndex = 0; };
            topPanel.Controls.Add(backBtn);
            var tsLabel = new Label { Text = "时间戳格式", Left = 130, Top = 10, Width = 80 };
            topPanel.Controls.Add(tsLabel);
            _timestampFormatBox = new ComboBox { Left = 215, Top = 6, Width = 220, DropDownStyle = ComboBoxStyle.DropDownList };
            _timestampFormatBox.Items.AddRange(new object[] { "HH:mm:ss.fff", "yyyy-MM-dd HH:mm:ss.fff", "O" });
            _timestampFormatBox.SelectedIndex = 0;
            topPanel.Controls.Add(_timestampFormatBox);
            left.Controls.Add(topPanel, 0, 0);

            var searchPanel = new Panel { Dock = DockStyle.Fill };
            StyleBandPanel(searchPanel);
            searchPanel.Controls.Add(new Label { Text = "信号查找", Left = 6, Top = 12, Width = 54 });
            _findVariableFieldBox = new ComboBox { Left = 64, Top = 8, Width = 94, DropDownStyle = ComboBoxStyle.DropDownList };
            _findVariableFieldBox.Items.AddRange(new object[] { "全部", "消息名", "信号名" });
            _findVariableFieldBox.SelectedIndex = 0;
            _findVariableFieldBox.SelectedIndexChanged += delegate { ApplyVariableFilter(); };
            searchPanel.Controls.Add(_findVariableFieldBox);
            _findVariableBox = new TextBox { Left = 164, Top = 8, Width = 200 };
            _findVariableBox.TextChanged += delegate { ApplyVariableFilter(); };
            _findVariableBox.KeyDown += delegate(object sender, KeyEventArgs e)
            {
                if (e.KeyCode == Keys.Enter)
                {
                    ApplyVariableFilter();
                    e.Handled = true;
                    e.SuppressKeyPress = true;
                }
            };
            searchPanel.Controls.Add(_findVariableBox);
            var findBtn = new Button { Text = "筛选", Left = 370, Top = 7, Width = 70, Height = 28 };
            findBtn.Click += delegate { ApplyVariableFilter(); };
            searchPanel.Controls.Add(findBtn);
            left.Controls.Add(searchPanel, 0, 1);

            _availableVarList = new ListBox();
            _availableVarList.Dock = DockStyle.Fill;
            left.Controls.Add(_availableVarList, 0, 2);

            var addPanel = new Panel { Dock = DockStyle.Fill };
            var addBtn = new Button { Text = "添加变量", Width = 100, Height = 28, Left = 6, Top = 5 };
            addBtn.Click += delegate { AddSelectedVariables(); };
            addPanel.Controls.Add(addBtn);
            var removeBtn = new Button { Text = "删除变量", Width = 100, Height = 28, Left = 116, Top = 5 };
            removeBtn.Click += delegate { RemoveSelectedVariable(); };
            addPanel.Controls.Add(removeBtn);
            left.Controls.Add(addPanel, 0, 3);

            _monitoredVarList = new CheckedListBox();
            _monitoredVarList.Dock = DockStyle.Fill;
            _monitoredVarList.CheckOnClick = true;
            _monitoredVarList.ItemCheck += delegate
            {
                BeginInvoke(new Action(delegate { RefreshChartSeriesBySelection(); }));
            };
            left.Controls.Add(_monitoredVarList, 0, 4);

            left.Controls.Add(new Label { Text = "实时变量监控（自动识别来源通道）", Dock = DockStyle.Fill, TextAlign = ContentAlignment.BottomLeft }, 0, 5);

            _realtimeGrid = new DataGridView();
            _realtimeGrid.Dock = DockStyle.Fill;
            _realtimeGrid.AutoGenerateColumns = true;
            _realtimeGrid.DataSource = _realtimeRows;
            left.Controls.Add(_realtimeGrid, 0, 6);

            split.Panel1.Controls.Add(left);

            var right = new TableLayoutPanel();
            right.Dock = DockStyle.Fill;
            right.ColumnCount = 1;
            right.RowCount = 2;
            right.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            right.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var rightTop = new Panel { Dock = DockStyle.Fill };
            StyleBandPanel(rightTop);
            rightTop.Controls.Add(new Label { Text = "曲线窗口（在左侧监控列表打钩后显示）", Left = 8, Top = 10, Width = 300 });
            right.Controls.Add(rightTop, 0, 0);

            _chart = new Chart { Dock = DockStyle.Fill };
            _chart.ChartAreas.Add(new ChartArea("main"));
            right.Controls.Add(_chart, 0, 1);

            split.Panel2.Controls.Add(right);
            page.Controls.Add(split);
            return page;
        }

        private TabPage BuildStorageTab()
        {
            var page = new TabPage("变量存储");
            var root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.ColumnCount = 1;
            root.RowCount = 2;
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var top = new Panel { Dock = DockStyle.Fill };
            StyleBandPanel(top);
            top.Controls.Add(new Label { Text = "查找", Left = 8, Top = 12, Width = 40 });
            _storageSearchFieldBox = new ComboBox { Left = 48, Top = 8, Width = 110, DropDownStyle = ComboBoxStyle.DropDownList };
            _storageSearchFieldBox.Items.AddRange(new object[] { "全部", "时间戳", "通道", "消息名", "信号名", "数值" });
            _storageSearchFieldBox.SelectedIndex = 0;
            _storageSearchFieldBox.SelectedIndexChanged += delegate { ApplyStorageFilter(); };
            top.Controls.Add(_storageSearchFieldBox);
            _storageSearchBox = new TextBox { Left = 164, Top = 8, Width = 220 };
            _storageSearchBox.TextChanged += delegate { ApplyStorageFilter(); };
            top.Controls.Add(_storageSearchBox);
            var btnFilter = new Button { Text = "筛选", Left = 390, Top = 7, Width = 68, Height = 28 };
            btnFilter.Click += delegate { ApplyStorageFilter(); };
            top.Controls.Add(btnFilter);
            var btnExport = new Button { Text = "导出日志", Left = 464, Top = 7, Width = 90, Height = 28 };
            btnExport.Click += delegate { ExportStorageLog(); };
            top.Controls.Add(btnExport);
            var btnImport = new Button { Text = "导入日志", Left = 560, Top = 7, Width = 90, Height = 28 };
            btnImport.Click += delegate { ImportStorageLog(); };
            top.Controls.Add(btnImport);
            root.Controls.Add(top, 0, 0);

            _grid = new DataGridView { Dock = DockStyle.Fill, AutoGenerateColumns = true, DataSource = _storageRows };
            root.Controls.Add(_grid, 0, 1);
            page.Controls.Add(root);
            return page;
        }

        private TabPage BuildBusMonitorTab()
        {
            var page = new TabPage("总线监控");
            var root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.ColumnCount = 1;
            root.RowCount = 3;
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var header = new Panel { Dock = DockStyle.Fill };
            header.Controls.Add(new Label
            {
                Text = "四通道总线原始监控（类似 candump）：展示方向、帧类型、错误状态与错误类型。",
                Left = 8,
                Top = 11,
                Width = 920
            });
            root.Controls.Add(header, 0, 0);

            var tools = new Panel { Dock = DockStyle.Fill };
            tools.Controls.Add(new Label { Text = "查找", Left = 8, Top = 12, Width = 40 });
            _busSearchFieldBox = new ComboBox { Left = 48, Top = 8, Width = 96, DropDownStyle = ComboBoxStyle.DropDownList };
            _busSearchFieldBox.Items.AddRange(new object[] { "全部", "时间", "方向", "类型", "ID", "DLC", "数据", "状态", "错误类型", "通道" });
            _busSearchFieldBox.SelectedIndex = 0;
            _busSearchFieldBox.SelectedIndexChanged += delegate { MarkBusViewDirty(true); };
            tools.Controls.Add(_busSearchFieldBox);
            _busSearchBox = new TextBox { Left = 148, Top = 8, Width = 180 };
            _busSearchBox.TextChanged += delegate { MarkBusViewDirty(true); };
            tools.Controls.Add(_busSearchBox);

            _busOnlyErrorBox = new CheckBox { Left = 334, Top = 10, Width = 90, Text = "仅错误帧" };
            _busOnlyErrorBox.CheckedChanged += delegate { MarkBusViewDirty(true); };
            tools.Controls.Add(_busOnlyErrorBox);

            tools.Controls.Add(new Label { Text = "排序", Left = 426, Top = 12, Width = 34 });
            _busSortBox = new ComboBox { Left = 462, Top = 8, Width = 130, DropDownStyle = ComboBoxStyle.DropDownList };
            _busSortBox.Items.AddRange(new object[] { "时间降序", "时间升序", "ID升序", "ID降序" });
            _busSortBox.SelectedIndex = 0;
            _busSortBox.SelectedIndexChanged += delegate { MarkBusViewDirty(true); };
            tools.Controls.Add(_busSortBox);

            _busChannelFilter = new CheckedListBox { Left = 598, Top = 6, Width = 150, Height = 30, CheckOnClick = true };
            _busChannelFilter.Items.Add("CH0", true);
            _busChannelFilter.Items.Add("CH1", true);
            _busChannelFilter.Items.Add("CH2", true);
            _busChannelFilter.Items.Add("CH3", true);
            _busChannelFilter.ItemCheck += delegate { BeginInvoke(new Action(delegate { MarkBusViewDirty(true); })); };
            tools.Controls.Add(_busChannelFilter);
            var btnBusRefresh = new Button { Left = 756, Top = 7, Width = 132, Height = 28, Text = "手动刷新/复位灯" };
            btnBusRefresh.Click += delegate
            {
                MarkBusViewDirty(true);
                ResetTrafficIndicators();
                SetStatus("总线视图已手动刷新，RX/TX 指示灯已复位。");
            };
            tools.Controls.Add(btnBusRefresh);
            var btnBusClear = new Button { Left = 896, Top = 7, Width = 100, Height = 28, Text = "清空日志" };
            btnBusClear.Click += delegate { ClearBusMonitorRows(); };
            tools.Controls.Add(btnBusClear);
            root.Controls.Add(tools, 0, 1);

            _busGrid = new DataGridView();
            _busGrid.Dock = DockStyle.Fill;
            _busGrid.AutoGenerateColumns = false;
            _busGrid.ReadOnly = true;
            _busGrid.AllowUserToAddRows = false;
            _busGrid.AllowUserToDeleteRows = false;
            _busGrid.RowHeadersVisible = false;
            _busGrid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
            _busGrid.DataSource = _busViewRows;

            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "Timestamp", HeaderText = "Time", Width = 110 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "Channel", HeaderText = "CH", Width = 46 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "Direction", HeaderText = "Dir", Width = 50 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "FrameType", HeaderText = "Type", Width = 64 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "IdHex", HeaderText = "ID", Width = 82 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "Dlc", HeaderText = "DLC", Width = 50 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "DataHex", HeaderText = "Data", Width = 360 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "ErrorState", HeaderText = "State", Width = 64 });
            _busGrid.Columns.Add(new DataGridViewTextBoxColumn { DataPropertyName = "ErrorType", HeaderText = "ErrorType", Width = 150 });
            _busGrid.CellFormatting += BusGridCellFormatting;
            root.Controls.Add(_busGrid, 0, 2);

            page.Controls.Add(root);
            return page;
        }

        private static void AddRow(TableLayoutPanel panel, int row, string label, Control control)
        {
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
            panel.Controls.Add(new Label { Text = label, TextAlign = ContentAlignment.MiddleLeft, Dock = DockStyle.Fill }, 0, row);
            panel.Controls.Add(control, 1, row);
        }

        private static Bitmap CreateStartIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.FillPolygon(Brushes.LimeGreen, new[] { new Point(4, 3), new Point(4, 13), new Point(13, 8) });
                g.DrawPolygon(Pens.DarkGreen, new[] { new Point(4, 3), new Point(4, 13), new Point(13, 8) });
            });
        }

        private static Bitmap CreateStopIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.FillRectangle(Brushes.Red, 4, 4, 9, 9);
                g.DrawRectangle(Pens.DarkRed, 4, 4, 9, 9);
            });
        }

        private static Bitmap CreateLinkIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.DrawEllipse(new Pen(Color.SteelBlue, 2), 2, 4, 7, 7);
                g.DrawEllipse(new Pen(Color.SteelBlue, 2), 7, 4, 7, 7);
            });
        }

        private static Bitmap CreateUnlinkIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.DrawEllipse(new Pen(Color.Gray, 2), 2, 4, 7, 7);
                g.DrawEllipse(new Pen(Color.Gray, 2), 7, 4, 7, 7);
                g.DrawLine(new Pen(Color.Red, 2), 3, 12, 13, 2);
            });
        }

        private static Bitmap CreateDocIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.FillRectangle(Brushes.White, 3, 2, 10, 12);
                g.DrawRectangle(Pens.DimGray, 3, 2, 10, 12);
                g.DrawLine(Pens.Gray, 5, 6, 11, 6);
                g.DrawLine(Pens.Gray, 5, 8, 11, 8);
                g.DrawLine(Pens.Gray, 5, 10, 11, 10);
            });
        }

        private static Bitmap CreateChartIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.DrawLine(new Pen(Color.Teal, 2), 2, 12, 6, 8);
                g.DrawLine(new Pen(Color.Teal, 2), 6, 8, 9, 10);
                g.DrawLine(new Pen(Color.Teal, 2), 9, 10, 13, 4);
            });
        }

        private static Bitmap CreateBusIcon()
        {
            return CreateGlyphIcon(delegate(Graphics g)
            {
                g.Clear(Color.Magenta);
                g.DrawRectangle(new Pen(Color.Peru, 2), 2, 4, 12, 7);
                g.FillEllipse(Brushes.Black, 4, 11, 2, 2);
                g.FillEllipse(Brushes.Black, 10, 11, 2, 2);
            });
        }

        private static Bitmap CreateGlyphIcon(Action<Graphics> paint)
        {
            var bmp = new Bitmap(16, 16);
            using (var g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                paint(g);
            }
            return bmp;
        }

        private Control BuildWorkspacePanel()
        {
            var panel = new TableLayoutPanel();
            panel.Dock = DockStyle.Fill;
            panel.ColumnCount = 1;
            panel.RowCount = 4;
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 86));
            panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));

            var titlePanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(4, 2, 4, 2) };
            var titleLabel = new Label
            {
                Text = "WorkSpace 工程列表",
                Dock = DockStyle.Fill,
                TextAlign = ContentAlignment.MiddleLeft
            };
            titlePanel.Controls.Add(titleLabel);
            panel.Controls.Add(titlePanel, 0, 0);

            var top = new TableLayoutPanel();
            top.Dock = DockStyle.Fill;
            top.ColumnCount = 3;
            top.RowCount = 2;
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));

            _workspacePathLabel = new Label
            {
                Dock = DockStyle.Fill,
                AutoEllipsis = true,
                BorderStyle = BorderStyle.FixedSingle,
                TextAlign = ContentAlignment.MiddleLeft
            };
            top.Controls.Add(_workspacePathLabel, 0, 0);
            top.SetColumnSpan(_workspacePathLabel, 2);

            var btnPick = new Button { Text = "选择目录", Dock = DockStyle.Fill };
            btnPick.Click += delegate { SelectWorkspaceFolder(); };
            top.Controls.Add(btnPick, 2, 0);
            var btnScan = new Button { Text = "刷新", Dock = DockStyle.Fill };
            btnScan.Click += delegate { RefreshWorkspaceProjects(); };
            top.Controls.Add(btnScan, 0, 1);
            var btnNew = new Button { Text = "新建工程", Dock = DockStyle.Fill };
            btnNew.Click += delegate { NewProject(); };
            top.Controls.Add(btnNew, 1, 1);
            var btnOpenDialog = new Button { Text = "浏览工程", Dock = DockStyle.Fill };
            btnOpenDialog.Click += delegate { OpenProject(); };
            top.Controls.Add(btnOpenDialog, 2, 1);
            panel.Controls.Add(top, 0, 1);

            var projectArea = new TableLayoutPanel();
            projectArea.Dock = DockStyle.Fill;
            projectArea.ColumnCount = 1;
            projectArea.RowCount = 2;
            projectArea.RowStyles.Add(new RowStyle(SizeType.Absolute, 220));
            projectArea.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            _workspaceProjectList = new ListBox();
            _workspaceProjectList.Dock = DockStyle.Fill;
            _workspaceProjectList.DoubleClick += delegate { StartSelectedWorkspaceProject(); };
            _workspaceProjectList.SelectedIndexChanged += delegate { UpdateActionStates(); };
            projectArea.Controls.Add(_workspaceProjectList, 0, 0);

            var watermarkPanel = new Panel
            {
                Dock = DockStyle.Fill,
                Margin = new Padding(16, 8, 16, 8),
                BackColor = Color.Transparent
            };
            _workspaceWatermarkCanvas = new Panel
            {
                Dock = DockStyle.Fill,
                BackColor = Color.Transparent
            };
            _workspaceWatermarkCanvas.Paint += delegate(object sender, PaintEventArgs e)
            {
                DrawWorkspaceVectorWatermark(e.Graphics, _workspaceWatermarkCanvas.ClientRectangle);
            };
            _workspaceWatermarkCanvas.Resize += delegate { _workspaceWatermarkCanvas.Invalidate(); };
            watermarkPanel.Controls.Add(_workspaceWatermarkCanvas);
            projectArea.Controls.Add(watermarkPanel, 0, 1);
            panel.Controls.Add(projectArea, 0, 2);

            panel.Controls.Add(new Label { Text = "提示：双击左侧工程或点地址栏“开始工程”。", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 3);

            return panel;
        }

        private void ApplyEverVanceIcon()
        {
            try
            {
                var iconPath = ResolveEverVanceAssetPath("EverVance.ico");
                if (string.IsNullOrWhiteSpace(iconPath) || !File.Exists(iconPath))
                {
                    return;
                }

                using (var raw = new Icon(iconPath))
                {
                    _appIcon = (Icon)raw.Clone();
                }
                Icon = _appIcon;
            }
            catch
            {
            }
        }

        private void DrawWorkspaceVectorWatermark(Graphics g, Rectangle bounds)
        {
            if (g == null || bounds.Width < 40 || bounds.Height < 40)
            {
                return;
            }

            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;

            var side = Math.Min(bounds.Width, bounds.Height) * 0.66f;
            var cx = bounds.Left + (bounds.Width / 2.0f);
            var cy = bounds.Top + (bounds.Height / 2.0f);
            var outer = new RectangleF(cx - side * 0.5f, cy - side * 0.5f, side, side);
            var mid = new RectangleF(cx - side * 0.42f, cy - side * 0.42f, side * 0.84f, side * 0.84f);
            var inner = new RectangleF(cx - side * 0.32f, cy - side * 0.32f, side * 0.64f, side * 0.64f);

            var fillColor = _darkTheme
                ? Color.FromArgb(18, 0, 210, 220)
                : Color.FromArgb(20, 0, 138, 195);
            var ringColor = _darkTheme
                ? Color.FromArgb(52, 0, 220, 230)
                : Color.FromArgb(56, 0, 128, 190);
            var ringColor2 = _darkTheme
                ? Color.FromArgb(42, 190, 240, 245)
                : Color.FromArgb(48, 120, 176, 212);
            var glyphColor = _darkTheme
                ? Color.FromArgb(84, 214, 248, 252)
                : Color.FromArgb(86, 24, 92, 136);

            using (var fill = new SolidBrush(fillColor))
            using (var p1 = new Pen(ringColor, Math.Max(2.0f, side * 0.038f)))
            using (var p2 = new Pen(ringColor2, Math.Max(1.6f, side * 0.022f)))
            {
                g.FillEllipse(fill, inner);
                g.DrawEllipse(p1, outer);
                g.DrawEllipse(p2, mid);
            }

            var stroke = Math.Max(2.2f, side * 0.065f);
            var eX = cx - side * 0.20f;
            var eTop = cy - side * 0.11f;
            var eBottom = cy + side * 0.11f;
            var eRight = cx - side * 0.04f;
            var vLeft = cx + side * 0.02f;
            var vMid = cx + side * 0.12f;
            var vRight = cx + side * 0.22f;

            using (var pen = new Pen(glyphColor, stroke) { StartCap = LineCap.Round, EndCap = LineCap.Round })
            {
                // E
                g.DrawLine(pen, eX, eTop, eX, eBottom);
                g.DrawLine(pen, eX, eTop, eRight, eTop);
                g.DrawLine(pen, eX, cy, eRight - side * 0.02f, cy);
                g.DrawLine(pen, eX, eBottom, eRight, eBottom);
                // V
                g.DrawLine(pen, vLeft, eTop, vMid, eBottom);
                g.DrawLine(pen, vMid, eBottom, vRight, eTop);
            }
        }

        private static string ResolveEverVanceAssetPath(string fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName))
            {
                return "";
            }

            var candidates = new List<string>();
            try
            {
                var baseDir = AppDomain.CurrentDomain.BaseDirectory ?? "";
                if (!string.IsNullOrWhiteSpace(baseDir))
                {
                    candidates.Add(Path.Combine(baseDir, "assets", fileName));
                    candidates.Add(Path.Combine(baseDir, "..", "assets", fileName));
                    candidates.Add(Path.Combine(baseDir, "..", "..", "assets", fileName));
                    candidates.Add(Path.Combine(baseDir, "..", "..", "..", "assets", fileName));
                }
            }
            catch
            {
            }

            try
            {
                var repo = TryFindRepoRoot();
                if (!string.IsNullOrWhiteSpace(repo))
                {
                    candidates.Add(Path.Combine(repo, "Host_PC", "EverVance", "assets", fileName));
                }
            }
            catch
            {
            }

            for (int i = 0; i < candidates.Count; i++)
            {
                try
                {
                    var full = Path.GetFullPath(candidates[i]);
                    if (File.Exists(full))
                    {
                        return full;
                    }
                }
                catch
                {
                }
            }

            return "";
        }

        private void SetProjectReady(bool ready)
        {
            _projectReady = ready;
            if (!ready)
            {
                StopTxSchedule();
            }
            UpdateActionStates();
            RenderIndicators();
        }

        private bool EnsureProjectReady()
        {
            if (_projectReady)
            {
                return true;
            }

            MessageBox.Show("请先新建工程或打开工程，然后再进行连接和调试。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return false;
        }

        private void UpdateActionStates()
        {
            var selectedPath = _workspaceProjectList == null || _workspaceProjectList.SelectedItem == null
                ? ""
                : _workspaceProjectList.SelectedItem.ToString();
            var selectedExists = !string.IsNullOrWhiteSpace(selectedPath) && File.Exists(selectedPath);
            var isSameProject = selectedExists && PathsEqual(selectedPath, _currentProjectPath);

            if (_tabs != null) _tabs.Enabled = true;
            if (_btnImportDbc != null) _btnImportDbc.Enabled = _projectReady;
            if (_btnMonitor != null) _btnMonitor.Enabled = _projectReady;
            if (_btnBus != null) _btnBus.Enabled = _projectReady;

            if (_btnStartProject != null)
            {
                _btnStartProject.Enabled = selectedExists && (!_projectReady || !isSameProject);
            }
            if (_btnEndProject != null) _btnEndProject.Enabled = _projectReady;

            var canConfigEndpoint = _projectReady && !_isConnected;
            if (_addressBox != null) _addressBox.Enabled = canConfigEndpoint;
            if (_endpointPresetBox != null) _endpointPresetBox.Enabled = canConfigEndpoint;
            if (_btnRefreshPorts != null) _btnRefreshPorts.Enabled = canConfigEndpoint;
            if (_btnConnect != null) _btnConnect.Enabled = _projectReady && !_isConnected;
            if (_btnDisconnect != null) _btnDisconnect.Enabled = _projectReady && _isConnected;
            UpdateChannelConfigEditState();
        }

        private static bool PathsEqual(string a, string b)
        {
            if (string.IsNullOrWhiteSpace(a) || string.IsNullOrWhiteSpace(b))
            {
                return false;
            }
            try
            {
                return string.Equals(
                    Path.GetFullPath(a).TrimEnd('\\'),
                    Path.GetFullPath(b).TrimEnd('\\'),
                    StringComparison.OrdinalIgnoreCase);
            }
            catch
            {
                return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
            }
        }

        private void UpdateChannelConfigEditState()
        {
            var canEdit = _projectReady && !_isConnected;

            if (_channelAddBox != null)
            {
                _channelAddBox.Enabled = canEdit;
            }
            if (_btnAddChannel != null)
            {
                _btnAddChannel.Enabled = canEdit;
            }
            if (_btnRemoveChannel != null)
            {
                _btnRemoveChannel.Enabled = canEdit;
            }
            if (_channelGrid != null)
            {
                _channelGrid.ReadOnly = !canEdit;
            }
        }

        private void NoteChannelConfigChanged()
        {
            _channelConfigSyncPending = true;
            if (!_isConnected)
            {
                SetStatus("通道配置已修改，请重新连接或重新下发到设备。");
            }
        }

        private void WireProjectDirtyTracking()
        {
            _channelConfigs.ListChanged += delegate { MarkProjectDirty(); };
            _txFrames.ListChanged += delegate { MarkProjectDirty(); };
            if (_addressBox != null)
            {
                _addressBox.TextChanged += delegate { MarkProjectDirty(); };
            }
            if (_monitoredVarList != null)
            {
                _monitoredVarList.ItemCheck += delegate
                {
                    BeginInvoke(new Action(delegate { MarkProjectDirty(); }));
                };
            }
        }

        private void MarkProjectDirty()
        {
            if (_suspendDirtyTracking)
            {
                return;
            }
            SetProjectDirty(true);
        }

        private void SetProjectDirty(bool dirty)
        {
            _projectDirty = dirty;
            UpdateWindowTitle();
        }

        private void UpdateWindowTitle()
        {
            Text = _projectDirty ? "EverVance *" : "EverVance";
        }

        private bool TryCloseCurrentProjectWithPrompt()
        {
            if (!_projectReady)
            {
                return true;
            }

            if (_projectDirty)
            {
                var dr = MessageBox.Show(
                    "当前工程有未保存更改，是否先保存再关闭？",
                    "EverVance",
                    MessageBoxButtons.YesNoCancel,
                    MessageBoxIcon.Question);
                if (dr == DialogResult.Cancel)
                {
                    return false;
                }
                if (dr == DialogResult.Yes && !SaveProjectWithResult(false))
                {
                    return false;
                }
            }

            DisconnectTransport();
            SetProjectReady(false);
            return true;
        }

        private void SelectWorkspaceFolder()
        {
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = "选择 WorkSpace 目录";
                dlg.SelectedPath = string.IsNullOrWhiteSpace(_workspacePath) ? Environment.CurrentDirectory : _workspacePath;
                if (dlg.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }
                SetWorkspacePath(dlg.SelectedPath);
                SaveLastWorkspacePath(dlg.SelectedPath);
            }
            RefreshWorkspaceProjects();
        }

        private bool PromptWorkspaceOnStartup()
        {
            var history = LoadWorkspaceHistory();
            var lastPath = LoadLastWorkspacePath();
            var initialPath = string.IsNullOrWhiteSpace(_workspacePath) ? lastPath : _workspacePath;

            using (var dlg = new Form())
            {
                dlg.Text = "EverVance - 选择 WorkSpace";
                dlg.StartPosition = FormStartPosition.CenterParent;
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.MinimizeBox = false;
                dlg.MaximizeBox = false;
                dlg.ShowInTaskbar = false;
                dlg.ClientSize = new Size(780, 240);
                dlg.Font = Font;

                var title = new Label
                {
                    Left = 16,
                    Top = 14,
                    Width = 748,
                    Height = 24,
                    Text = "请选择 WorkSpace 目录",
                    Font = new Font(Font, FontStyle.Bold)
                };
                dlg.Controls.Add(title);

                var hint = new Label
                {
                    Left = 16,
                    Top = 42,
                    Width = 748,
                    Height = 20,
                    Text = "支持从下拉菜单快速选择历史路径，也可点击“浏览...”选择新路径。"
                };
                dlg.Controls.Add(hint);

                var lastPathLabel = new Label
                {
                    Left = 16,
                    Top = 66,
                    Width = 748,
                    Height = 20,
                    AutoEllipsis = true,
                    Text = "上次打开: " + (string.IsNullOrWhiteSpace(lastPath) ? "(无)" : lastPath)
                };
                dlg.Controls.Add(lastPathLabel);

                var pathLabel = new Label
                {
                    Left = 16,
                    Top = 94,
                    Width = 120,
                    Height = 22,
                    Text = "WorkSpace 路径"
                };
                dlg.Controls.Add(pathLabel);

                var pathBox = new ComboBox
                {
                    Left = 16,
                    Top = 118,
                    Width = 640,
                    Height = 28,
                    DropDownStyle = ComboBoxStyle.DropDown,
                    IntegralHeight = false,
                    DropDownHeight = 220
                };
                pathBox.AutoCompleteMode = AutoCompleteMode.SuggestAppend;
                pathBox.AutoCompleteSource = AutoCompleteSource.ListItems;
                for (int i = 0; i < history.Count; i++)
                {
                    pathBox.Items.Add(history[i]);
                }
                pathBox.Text = string.IsNullOrWhiteSpace(initialPath) ? Environment.CurrentDirectory : initialPath;
                dlg.Controls.Add(pathBox);

                var browseBtn = new Button
                {
                    Left = 664,
                    Top = 117,
                    Width = 100,
                    Height = 30,
                    Text = "浏览..."
                };
                browseBtn.Click += delegate
                {
                    using (var pick = new FolderBrowserDialog())
                    {
                        pick.Description = "选择 WorkSpace 目录";
                        pick.SelectedPath = string.IsNullOrWhiteSpace(pathBox.Text) ? Environment.CurrentDirectory : pathBox.Text;
                        if (pick.ShowDialog(dlg) == DialogResult.OK)
                        {
                            pathBox.Text = pick.SelectedPath;
                        }
                    }
                };
                dlg.Controls.Add(browseBtn);

                var openBtn = new Button
                {
                    Left = 568,
                    Top = 184,
                    Width = 95,
                    Height = 32,
                    Text = "打开"
                };
                var closeBtn = new Button
                {
                    Left = 669,
                    Top = 184,
                    Width = 95,
                    Height = 32,
                    Text = "关闭"
                };

                openBtn.Click += delegate
                {
                    var selected = (pathBox.Text ?? "").Trim();
                    if (string.IsNullOrWhiteSpace(selected))
                    {
                        MessageBox.Show("请先选择有效的 WorkSpace 路径。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        pathBox.Focus();
                        return;
                    }
                    if (!Directory.Exists(selected))
                    {
                        MessageBox.Show("所选路径不存在，请重新选择。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        pathBox.Focus();
                        return;
                    }

                    SetWorkspacePath(selected);
                    SaveLastWorkspacePath(selected);
                    dlg.DialogResult = DialogResult.OK;
                    dlg.Close();
                };

                closeBtn.Click += delegate
                {
                    dlg.DialogResult = DialogResult.Cancel;
                    dlg.Close();
                };

                dlg.Controls.Add(openBtn);
                dlg.Controls.Add(closeBtn);
                dlg.AcceptButton = openBtn;
                dlg.CancelButton = closeBtn;

                return dlg.ShowDialog(this) == DialogResult.OK;
            }
        }

        private string LoadLastWorkspacePath()
        {
            try
            {
                string path;
                if (_appState != null && _appState.TryGetValue("WorkspacePath", out path))
                {
                    if (!string.IsNullOrWhiteSpace(path))
                    {
                        return path;
                    }
                }

                if (File.Exists(_appStatePath))
                {
                    var line = File.ReadAllText(_appStatePath, Encoding.UTF8).Trim();
                    if (!string.IsNullOrWhiteSpace(line) && line.IndexOf('=') < 0)
                    {
                        return line;
                    }
                }
            }
            catch
            {
            }
            return Environment.CurrentDirectory;
        }

        private List<string> LoadWorkspaceHistory()
        {
            var history = new List<string>();
            try
            {
                string raw;
                if (_appState != null && _appState.TryGetValue("WorkspaceHistory", out raw))
                {
                    var parts = (raw ?? "").Split(new[] { '|' }, StringSplitOptions.RemoveEmptyEntries);
                    for (int i = 0; i < parts.Length; i++)
                    {
                        var decoded = DecodeProjectValue(parts[i]);
                        if (string.IsNullOrWhiteSpace(decoded))
                        {
                            continue;
                        }
                        if (!history.Any(h => PathsEqual(h, decoded)))
                        {
                            history.Add(decoded);
                        }
                    }
                }
            }
            catch
            {
            }

            var last = LoadLastWorkspacePath();
            if (!string.IsNullOrWhiteSpace(last) && !history.Any(h => PathsEqual(h, last)))
            {
                history.Insert(0, last);
            }

            if (history.Count > 12)
            {
                history = history.Take(12).ToList();
            }
            return history;
        }

        private void SetWorkspacePath(string path)
        {
            _workspacePath = path ?? "";
            if (_workspacePathLabel != null)
            {
                _workspacePathLabel.Text = _workspacePath;
                _workspacePathTip.SetToolTip(_workspacePathLabel, _workspacePath);
            }
        }

        private void RefreshEndpointPresets(bool keepCurrentAddress)
        {
            if (_endpointPresetBox == null)
            {
                return;
            }

            var current = _addressBox == null ? "" : _addressBox.Text;
            _endpointPresetBox.Items.Clear();
            _endpointPresetBox.Items.Add("Mock(100ms) | mock://localhost?period=100");
            _endpointPresetBox.Items.Add("Mock(200ms) | mock://localhost?period=200");
            _endpointPresetBox.Items.Add("WinUSB Auto | winusb://auto");
            _endpointPresetBox.Items.Add("WinUSB NXP 示例 VID=1FC9 PID=0135 | winusb://vid=0x1FC9&pid=0x0135");
            var devs = WinUsbTransport.EnumeratePresentDevices();
            for (int i = 0; i < devs.Count; i++)
            {
                var d = devs[i];
                if (d.Vid >= 0 && d.Pid >= 0)
                {
                    _endpointPresetBox.Items.Add(
                        string.Format("WinUSB Device VID={0:X4} PID={1:X4} | winusb://vid=0x{0:X4}&pid=0x{1:X4}", d.Vid, d.Pid));
                }
                else
                {
                    _endpointPresetBox.Items.Add("WinUSB Device(Unknown VID/PID) | winusb://auto");
                }
            }

            if (keepCurrentAddress && !string.IsNullOrWhiteSpace(current))
            {
                _addressBox.Text = current;
            }

            if (_endpointPresetBox.Items.Count > 0 && _endpointPresetBox.SelectedIndex < 0)
            {
                _endpointPresetBox.SelectedIndex = 0;
            }
        }

        private void ApplyEndpointPresetSelection()
        {
            if (_endpointPresetBox == null || _addressBox == null || _endpointPresetBox.SelectedItem == null)
            {
                return;
            }

            var s = _endpointPresetBox.SelectedItem.ToString();
            var idx = s.IndexOf('|');
            if (idx < 0)
            {
                return;
            }
            _addressBox.Text = s.Substring(idx + 1).Trim();
        }

        private void SaveLastWorkspacePath(string path)
        {
            try
            {
                if (_appState == null)
                {
                    _appState = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                }
                var selected = (path ?? "").Trim();
                _appState["WorkspacePath"] = selected;

                var history = LoadWorkspaceHistory();
                for (int i = history.Count - 1; i >= 0; i--)
                {
                    if (PathsEqual(history[i], selected))
                    {
                        history.RemoveAt(i);
                    }
                }
                if (!string.IsNullOrWhiteSpace(selected))
                {
                    history.Insert(0, selected);
                }
                if (history.Count > 12)
                {
                    history = history.Take(12).ToList();
                }
                _appState["WorkspaceHistory"] = string.Join("|", history.Where(h => !string.IsNullOrWhiteSpace(h)).Select(EncodeProjectValue).ToArray());
                SaveAppState();
            }
            catch
            {
            }
        }

        private void LoadAppState()
        {
            _appState = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                if (!File.Exists(_appStatePath))
                {
                    return;
                }
                var lines = File.ReadAllLines(_appStatePath, Encoding.UTF8);
                for (int i = 0; i < lines.Length; i++)
                {
                    var line = lines[i].Trim();
                    if (line.Length == 0) continue;
                    var idx = line.IndexOf('=');
                    if (idx <= 0) continue;
                    _appState[line.Substring(0, idx)] = line.Substring(idx + 1);
                }
            }
            catch
            {
            }
        }

        private void SaveAppState()
        {
            try
            {
                var dir = Path.GetDirectoryName(_appStatePath);
                if (!Directory.Exists(dir))
                {
                    Directory.CreateDirectory(dir);
                }
                var lines = _appState.Select(kv => kv.Key + "=" + kv.Value).ToArray();
                File.WriteAllLines(_appStatePath, lines, Encoding.UTF8);
            }
            catch
            {
            }
        }

        private void ApplyLayoutFromAppState()
        {
            if (_appState == null || _appState.Count == 0)
            {
                return;
            }

            int x = ParseIntSafe(GetApp("WindowX"), Left);
            int y = ParseIntSafe(GetApp("WindowY"), Top);
            int w = ParseIntSafe(GetApp("WindowW"), Width);
            int h = ParseIntSafe(GetApp("WindowH"), Height);
            if (w >= MinimumSize.Width && h >= MinimumSize.Height)
            {
                StartPosition = FormStartPosition.Manual;
                Left = x;
                Top = y;
                Width = w;
                Height = h;
            }

            if (_mainSplit != null)
            {
                var d = ParseIntSafe(GetApp("MainSplit"), _mainSplit.SplitterDistance);
                _mainSplit.SplitterDistance = Math.Max(_mainSplit.Panel1MinSize, Math.Min(_mainSplit.Width - 520, d));
            }
            if (_plotSplit != null)
            {
                var d = ParseIntSafe(GetApp("PlotSplit"), _plotSplit.SplitterDistance);
                _plotSplit.SplitterDistance = Math.Max(320, Math.Min(_plotSplit.Width - 320, d));
            }
        }

        private string GetApp(string key)
        {
            string v;
            return (_appState != null && _appState.TryGetValue(key, out v)) ? v : "";
        }

        private void RefreshWorkspaceProjects()
        {
            if (_workspaceProjectList == null)
            {
                return;
            }

            _workspaceProjectList.Items.Clear();
            var root = _workspacePath;
            if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root))
            {
                return;
            }

            var files = Directory.GetFiles(root, "*.evproj", SearchOption.TopDirectoryOnly).OrderBy(p => p).ToList();
            foreach (var f in files)
            {
                _workspaceProjectList.Items.Add(f);
            }
            UpdateActionStates();
        }

        private void StartSelectedWorkspaceProject()
        {
            if (_workspaceProjectList == null || _workspaceProjectList.SelectedItem == null)
            {
                OpenProject();
                return;
            }

            var path = _workspaceProjectList.SelectedItem.ToString();
            if (!File.Exists(path))
            {
                MessageBox.Show("选中的工程文件不存在。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                RefreshWorkspaceProjects();
                return;
            }
            if (_projectReady && PathsEqual(path, _currentProjectPath))
            {
                SetStatus("当前工程已打开。若需切换，请先选择其他工程。");
                UpdateActionStates();
                return;
            }
            if (!TryCloseCurrentProjectWithPrompt())
            {
                return;
            }
            LoadProjectFromFile(path);
        }

        private void EndCurrentProject()
        {
            if (!TryCloseCurrentProjectWithPrompt())
            {
                return;
            }
            SetStatus("工程已结束。请在左侧 WorkSpace 选择工程后点击“开始工程”。");
        }

        private void ConnectTransport()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            DisconnectTransport();
            _transport = TransportFactory.Create(_addressBox.Text);
            if (_transport.Open(_addressBox.Text))
            {
                _isConnected = true;
                _lastDeviceActivityUtc = DateTime.UtcNow;
                _status.Text = _channelConfigSyncPending
                    ? string.Format("已连接: {0}，检测到离线期间的通道配置修改，正在重新同步。", _transport.Name)
                    : string.Format("已连接: {0}", _transport.Name);
                AutoQueryDeviceOnConnect();
                AutoSendChannelConfigsOnConnect();
                _linkHeartbeatTimer.Start();
                SendBackgroundHeartbeat();
            }
            else
            {
                _isConnected = false;
                _status.Text = "连接失败";
            }
            UpdateActionStates();
            RenderIndicators();
        }

        private void AutoSendChannelConfigsOnConnect()
        {
            int success = 0;
            int total = 0;

            foreach (var row in _channelConfigs.OrderBy(c => c.ChannelId))
            {
                total++;
                if (SendChannelConfigRow(row, true))
                {
                    success++;
                }
            }

            if (total == 0)
            {
                _channelConfigSyncPending = false;
                SetStatus(string.Format("已连接: {0}，当前无通道配置需要同步。", _transport.Name));
                return;
            }

            if (success == total)
            {
                _channelConfigSyncPending = false;
                SetStatus(string.Format("已连接: {0}，通道参数已自动同步 {1}/{1}。", _transport.Name, total));
                return;
            }

            SetStatus(string.Format("已连接: {0}，通道参数自动同步 {1}/{2}。请检查采样点或波特率配置。", _transport.Name, success, total));
        }

        private void AutoQueryDeviceOnConnect()
        {
            int i;

            SendControlQuery(0, ProtocolCmdGetDeviceInfo, 0x10, null);
            SendControlQuery(0, ProtocolCmdHeartbeat, 0x11, new byte[] { 0x56, 0x42, 0x41, 0x31 });

            for (i = 0; i < 4; i++)
            {
                SendControlQuery((byte)i, ProtocolCmdGetChannelCapabilities, (byte)(0x20 + i), null);
                SendControlQuery((byte)i, ProtocolCmdGetRuntimeStatus, (byte)(0x30 + i), null);
                SendControlQuery((byte)i, ProtocolCmdGetChannelConfig, (byte)(0x40 + i), null);
            }
        }

        private void DisconnectTransport(bool notifyDevice = true)
        {
            _linkHeartbeatTimer.Stop();
            if (_transport != null)
            {
                if (notifyDevice && _isConnected)
                {
                    try
                    {
                        _transport.Send(BuildControlPacket(0, ProtocolCmdHeartbeat, 0, _linkHeartbeatSequence++, UnlinkHeartbeatPayload));
                    }
                    catch
                    {
                    }
                }
                _transport.Close();
            }
            _isConnected = false;
            _lastDeviceActivityUtc = DateTime.MinValue;
            _status.Text = _channelConfigSyncPending ? "已断开，通道配置有未同步改动。" : "已断开";
            UpdateActionStates();
            RenderIndicators();
        }

        private void HandleTransportLost(string reason)
        {
            if (!_isConnected)
            {
                return;
            }

            DisconnectTransport(false);
            _status.Text = reason;
            RenderIndicators();
        }

        private bool TrySendPacket(byte[] packet)
        {
            if (!_isConnected || packet == null)
            {
                return false;
            }

            if (_transport.Send(packet))
            {
                return true;
            }

            HandleTransportLost("设备已断开，连接状态已自动更新。");
            return false;
        }

        private void SendBackgroundHeartbeat()
        {
            if (!_isConnected)
            {
                _linkHeartbeatTimer.Stop();
                return;
            }
            if (_lastDeviceActivityUtc != DateTime.MinValue &&
                (DateTime.UtcNow - _lastDeviceActivityUtc).TotalSeconds > 5.0)
            {
                HandleTransportLost("设备已断开，连接状态已自动更新。");
                return;
            }

            SendControlQuery(0, ProtocolCmdHeartbeat, _linkHeartbeatSequence++, LinkHeartbeatPayload);
        }

        private void AddChannelConfig()
        {
            if (_isConnected)
            {
                SetStatus("设备已连接，通道配置已锁定。请先断开连接后再修改。");
                return;
            }
            try
            {
                // 从下拉框取目标通道并创建配置行
                if (_channelAddBox == null || _channelAddBox.SelectedItem == null)
                {
                    return;
                }
                var id = int.Parse(_channelAddBox.SelectedItem.ToString(), CultureInfo.InvariantCulture);
                AddChannelConfig(id);
            }
            catch (Exception ex)
            {
                MessageBox.Show("添加通道失败: " + ex.Message, "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void AddChannelConfig(int channelId)
        {
            // 同一通道仅允许一条配置，避免误操作重复发送
            if (_channelConfigs.Any(c => c.ChannelId == channelId))
            {
                SetStatus(string.Format("通道 CH{0} 已存在", channelId));
                return;
            }

            _channelConfigs.Add(new ChannelConfigRow
            {
                ChannelId = channelId,
                Enabled = true,
                FrameType = FrameTypeClassic,
                NominalBitrate = 500000,
                NominalSamplePreset = "80.0%",
                NominalSamplePointText = "80.0",
                DataBitrate = 2000000,
                DataSamplePreset = "75.0%",
                DataSamplePointText = "75.0",
                TerminationEnabled = false
            });
            NormalizeChannelConfigRow(_channelConfigs[_channelConfigs.Count - 1]);
            SortChannelConfigs();
            if (_projectReady && !_suspendDirtyTracking)
            {
                NoteChannelConfigChanged();
            }
            SetStatus(string.Format("已添加通道 CH{0}", channelId));
        }

        private void RemoveSelectedChannelConfig()
        {
            if (_isConnected)
            {
                SetStatus("设备已连接，通道配置已锁定。请先断开连接后再修改。");
                return;
            }
            // 删除当前选中的通道配置行
            if (_channelGrid == null || _channelGrid.CurrentRow == null || _channelGrid.CurrentRow.Index < 0)
            {
                return;
            }

            var row = _channelGrid.CurrentRow.DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            _channelConfigs.Remove(row);
            if (_projectReady && !_suspendDirtyTracking)
            {
                NoteChannelConfigChanged();
            }
            SetStatus(string.Format("已删除通道 CH{0}", row.ChannelId));
        }

        private void SortChannelConfigs()
        {
            // 保持按通道号排序，方便用户阅读和批量发送
            if (_channelConfigs.Count <= 1)
            {
                return;
            }

            var sorted = _channelConfigs.OrderBy(c => c.ChannelId).ToList();
            _channelConfigs.RaiseListChangedEvents = false;
            _channelConfigs.Clear();
            foreach (var c in sorted)
            {
                _channelConfigs.Add(c);
            }
            _channelConfigs.RaiseListChangedEvents = true;
            _channelConfigs.ResetBindings();
        }

        private static bool IsCanFdFrameType(string frameType)
        {
            return string.Equals((frameType ?? "").Trim(), FrameTypeFd, StringComparison.OrdinalIgnoreCase);
        }

        private static string NormalizeSamplePreset(string preset, bool dataPhase)
        {
            var valid = dataPhase ? DataSamplePresetItems : NominalSamplePresetItems;
            if (string.IsNullOrWhiteSpace(preset))
            {
                return valid[1];
            }

            foreach (var item in valid)
            {
                if (string.Equals(item, preset.Trim(), StringComparison.OrdinalIgnoreCase))
                {
                    return item;
                }
            }
            return SamplePresetCustom;
        }

        private static string PresetToSampleText(string preset, bool dataPhase)
        {
            var normalized = NormalizeSamplePreset(preset, dataPhase);
            if (string.Equals(normalized, SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
            {
                return dataPhase ? "75.0" : "80.0";
            }

            return normalized.TrimEnd('%');
        }

        private static string NormalizeSampleText(string value, string preset, bool dataPhase)
        {
            if (!string.Equals(NormalizeSamplePreset(preset, dataPhase), SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
            {
                return PresetToSampleText(preset, dataPhase);
            }

            double parsed;
            if (!TryParseSamplePercent(value, out parsed))
            {
                return dataPhase ? "75.0" : "80.0";
            }

            return parsed.ToString("0.0", CultureInfo.InvariantCulture);
        }

        private static bool TryParseSamplePercent(string text, out double percent)
        {
            var normalized = (text ?? "").Trim().Replace("%", "");
            return double.TryParse(normalized, NumberStyles.Float, CultureInfo.InvariantCulture, out percent);
        }

        private static bool TryParseSamplePermille(string text, string preset, bool dataPhase, out ushort permille, out string error)
        {
            double percent;
            double minPercent = dataPhase ? 50.0 : 50.0;
            double maxPercent = dataPhase ? 85.0 : 90.0;

            error = null;
            permille = 0;

            if (!TryParseSamplePercent(NormalizeSampleText(text, preset, dataPhase), out percent))
            {
                error = dataPhase ? "数据域采样点格式无效。" : "仲裁域采样点格式无效。";
                return false;
            }
            if (percent < minPercent || percent > maxPercent)
            {
                error = string.Format(CultureInfo.InvariantCulture,
                    "{0}采样点需在 {1:0.0}% ~ {2:0.0}% 之间。",
                    dataPhase ? "数据域" : "仲裁域",
                    minPercent,
                    maxPercent);
                return false;
            }

            permille = (ushort)Math.Round(percent * 10.0, MidpointRounding.AwayFromZero);
            return true;
        }

        private static void NormalizeChannelConfigRow(ChannelConfigRow row)
        {
            if (row == null)
            {
                return;
            }

            row.FrameType = IsCanFdFrameType(row.FrameType) ? FrameTypeFd : FrameTypeClassic;
            if (row.NominalBitrate <= 0)
            {
                row.NominalBitrate = 500000;
            }

            row.NominalSamplePreset = NormalizeSamplePreset(row.NominalSamplePreset, false);
            row.NominalSamplePointText = NormalizeSampleText(row.NominalSamplePointText, row.NominalSamplePreset, false);

            if (!IsCanFdFrameType(row.FrameType))
            {
                row.DataBitrate = 0;
            }
            else if (row.DataBitrate <= 0)
            {
                row.DataBitrate = 2000000;
            }

            row.DataSamplePreset = NormalizeSamplePreset(row.DataSamplePreset, true);
            row.DataSamplePointText = NormalizeSampleText(row.DataSamplePointText, row.DataSamplePreset, true);
        }

        private bool TryBuildChannelConfigPayload(ChannelConfigRow row, out byte[] payload, out string error)
        {
            ushort nominalSamplePermille;
            ushort dataSamplePermille;
            bool isCanFd;

            payload = null;
            error = null;

            if (row == null)
            {
                error = "通道配置为空。";
                return false;
            }

            NormalizeChannelConfigRow(row);
            isCanFd = IsCanFdFrameType(row.FrameType);

            if (row.NominalBitrate <= 0)
            {
                error = string.Format(CultureInfo.InvariantCulture, "CH{0} 仲裁域波特率必须大于 0。", row.ChannelId);
                return false;
            }
            if (!TryParseSamplePermille(row.NominalSamplePointText, row.NominalSamplePreset, false, out nominalSamplePermille, out error))
            {
                error = string.Format(CultureInfo.InvariantCulture, "CH{0} {1}", row.ChannelId, error);
                return false;
            }

            dataSamplePermille = 0;
            if (isCanFd)
            {
                if (row.DataBitrate <= 0)
                {
                    error = string.Format(CultureInfo.InvariantCulture, "CH{0} 为 CAN FD 时，数据域波特率必须大于 0。", row.ChannelId);
                    return false;
                }
                if (!TryParseSamplePermille(row.DataSamplePointText, row.DataSamplePreset, true, out dataSamplePermille, out error))
                {
                    error = string.Format(CultureInfo.InvariantCulture, "CH{0} {1}", row.ChannelId, error);
                    return false;
                }
            }

            payload = new byte[16];
            payload[0] = ProtocolVersion;
            payload[1] = (byte)(isCanFd ? 1 : 0);
            payload[2] = (byte)(row.Enabled ? 1 : 0);
            payload[3] = (byte)(row.TerminationEnabled ? 1 : 0);
            WriteUInt32LittleEndian(payload, 4, (uint)row.NominalBitrate);
            WriteUInt16LittleEndian(payload, 8, nominalSamplePermille);
            WriteUInt32LittleEndian(payload, 10, (uint)(isCanFd ? row.DataBitrate : 0));
            WriteUInt16LittleEndian(payload, 14, (ushort)(isCanFd ? dataSamplePermille : 0));
            return true;
        }

        private static void WriteUInt32LittleEndian(byte[] buffer, int offset, uint value)
        {
            buffer[offset + 0] = (byte)(value & 0xFF);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
            buffer[offset + 2] = (byte)((value >> 16) & 0xFF);
            buffer[offset + 3] = (byte)((value >> 24) & 0xFF);
        }

        private static void WriteUInt16LittleEndian(byte[] buffer, int offset, ushort value)
        {
            buffer[offset + 0] = (byte)(value & 0xFF);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
        }

        private static uint ReadUInt32LittleEndian(byte[] buffer, int offset)
        {
            return (uint)(buffer[offset + 0] |
                (buffer[offset + 1] << 8) |
                (buffer[offset + 2] << 16) |
                (buffer[offset + 3] << 24));
        }

        private static ushort ReadUInt16LittleEndian(byte[] buffer, int offset)
        {
            return (ushort)(buffer[offset + 0] | (buffer[offset + 1] << 8));
        }

        private static byte[] BuildControlPacket(byte channel, byte command, byte status, byte sequence, byte[] payload)
        {
            var body = payload ?? new byte[0];
            var buf = new byte[8 + body.Length];
            buf[0] = PacketSync;
            buf[1] = channel;
            buf[2] = (byte)body.Length;
            buf[3] = PacketFlagControl;
            buf[4] = command;
            buf[5] = status;
            buf[6] = sequence;
            buf[7] = ProtocolVersion;
            if (body.Length > 0)
            {
                Buffer.BlockCopy(body, 0, buf, 8, body.Length);
            }
            return buf;
        }

        private bool SendControlQuery(byte channel, byte command, byte sequence, byte[] payload)
        {
            if (!_isConnected)
            {
                return false;
            }

            return TrySendPacket(BuildControlPacket(channel, command, 0, sequence, payload ?? new byte[0]));
        }

        private bool SendChannelConfigRow(ChannelConfigRow row, bool silent)
        {
            byte[] payload;
            string error;

            if (!_isConnected)
            {
                if (!silent)
                {
                    SetStatus("请先连接设备，再同步通道配置。");
                }
                return false;
            }
            if (!TryBuildChannelConfigPayload(row, out payload, out error))
            {
                if (!silent)
                {
                    SetStatus(error);
                }
                return false;
            }

            if (!TrySendPacket(BuildControlPacket((byte)row.ChannelId, ProtocolCmdSetChannelConfig, 0, (byte)row.ChannelId, payload)))
            {
                if (!silent)
                {
                    SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置下发失败。", row.ChannelId));
                }
                return false;
            }

            if (!silent)
            {
                SetStatus(string.Format(CultureInfo.InvariantCulture,
                    "已下发 CH{0} 配置: {1}, N={2}, NSP={3}{4}",
                    row.ChannelId,
                    IsCanFdFrameType(row.FrameType) ? FrameTypeFd : FrameTypeClassic,
                    row.NominalBitrate,
                    NormalizeSampleText(row.NominalSamplePointText, row.NominalSamplePreset, false),
                    row.TerminationEnabled ? ", 终端=开" : ", 终端=关"));
            }
            return true;
        }

        private void AddTxFrameRow()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            if (_txFrameChannelAddBox == null || _txFrameChannelAddBox.SelectedItem == null)
            {
                return;
            }
            int channelId = int.Parse(_txFrameChannelAddBox.SelectedItem.ToString(), CultureInfo.InvariantCulture);
            AddTxFrameRow(channelId);
        }

        private void AddTxFrameRow(int channelId)
        {
            _txFrames.Add(new TxFrameRow
            {
                Enabled = true,
                ChannelId = channelId,
                IdHex = "123",
                DataHex = "11 22 33 44",
                IntervalMs = 100,
                Note = "",
                LastSentUtc = DateTime.MinValue
            });
            SetStatus(string.Format("已添加发送帧（CH{0}）", channelId));
        }

        private void RemoveSelectedTxFrameRow()
        {
            if (_txFrameGrid == null || _txFrameGrid.CurrentRow == null || _txFrameGrid.CurrentRow.Index < 0)
            {
                return;
            }
            var row = _txFrameGrid.CurrentRow.DataBoundItem as TxFrameRow;
            if (row == null)
            {
                return;
            }
            _txFrames.Remove(row);
            SetStatus("已删除发送帧");
        }

        private void SendSelectedTxFrame()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            if (_txFrameGrid == null || _txFrameGrid.CurrentRow == null || _txFrameGrid.CurrentRow.Index < 0)
            {
                SetStatus("请先在发送帧列表中选择一行。");
                return;
            }
            var row = _txFrameGrid.CurrentRow.DataBoundItem as TxFrameRow;
            if (row == null)
            {
                return;
            }
            if (SendTxFrameRow(row))
            {
                row.LastSentUtc = DateTime.UtcNow;
            }
        }

        private void SendAllEnabledTxFrames()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            var rows = _txFrames.Where(f => f.Enabled).ToList();
            if (rows.Count == 0)
            {
                SetStatus("没有启用的发送帧。");
                return;
            }

            int okCount = 0;
            foreach (var row in rows)
            {
                if (SendTxFrameRow(row))
                {
                    row.LastSentUtc = DateTime.UtcNow;
                    okCount++;
                }
            }
            SetStatus(string.Format("发送完成: 成功 {0}/{1}", okCount, rows.Count));
        }

        private void StartTxSchedule()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            _txScheduleRunning = true;
            _txScheduleTimer.Start();
            SetStatus("已启动定时发送");
        }

        private void StopTxSchedule()
        {
            _txScheduleRunning = false;
            _txScheduleTimer.Stop();
            SetStatus("已停止定时发送");
        }

        private void OnTxScheduleTick()
        {
            if (!_txScheduleRunning)
            {
                return;
            }

            var now = DateTime.UtcNow;
            foreach (var row in _txFrames)
            {
                if (!row.Enabled || row.IntervalMs <= 0)
                {
                    continue;
                }

                var elapsed = (now - row.LastSentUtc).TotalMilliseconds;
                if (row.LastSentUtc == DateTime.MinValue || elapsed >= row.IntervalMs)
                {
                    if (SendTxFrameRow(row))
                    {
                        row.LastSentUtc = now;
                    }
                }
            }
        }

        private void MainForm_KeyDown(object sender, KeyEventArgs e)
        {
            // Windows 常用快捷键
            if (e.Control && e.KeyCode == Keys.N)
            {
                NewProject();
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (e.Control && e.KeyCode == Keys.O)
            {
                OpenProject();
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (e.Control && e.Shift && e.KeyCode == Keys.S)
            {
                SaveProjectAs();
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (e.Control && e.KeyCode == Keys.S)
            {
                if (EnsureProjectReady())
                {
                    SaveProject();
                }
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (e.KeyCode == Keys.F5)
            {
                if (EnsureProjectReady())
                {
                    ConnectTransport();
                }
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }

            if (e.Control && e.KeyCode == Keys.F)
            {
                if (_tabs == null)
                {
                    return;
                }

                // 按当前窗口就地查找，不再跳转到其他页面
                if (_tabs.SelectedIndex == 2 && _findVariableBox != null)
                {
                    _findVariableBox.Focus();
                    _findVariableBox.SelectAll();
                }
                else if (_tabs.SelectedIndex == 3 && _busSearchBox != null)
                {
                    _busSearchBox.Focus();
                    _busSearchBox.SelectAll();
                }
                else if (_tabs.SelectedIndex == 4 && _storageSearchBox != null)
                {
                    _storageSearchBox.Focus();
                    _storageSearchBox.SelectAll();
                }
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }

            // 调试快捷键：Alt+1..9 发送第 N 条启用帧（单次）
            if (!e.Alt)
            {
                return;
            }

            int index = -1;
            if (e.KeyCode >= Keys.D1 && e.KeyCode <= Keys.D9)
            {
                index = (int)(e.KeyCode - Keys.D1);
            }
            else if (e.KeyCode >= Keys.NumPad1 && e.KeyCode <= Keys.NumPad9)
            {
                index = (int)(e.KeyCode - Keys.NumPad1);
            }

            if (index < 0)
            {
                return;
            }

            SendEnabledFrameByIndex(index);
            e.Handled = true;
            e.SuppressKeyPress = true;
        }

        private void SendEnabledFrameByIndex(int enabledIndex)
        {
            var rows = _txFrames.Where(f => f.Enabled).ToList();
            if (enabledIndex < 0 || enabledIndex >= rows.Count)
            {
                SetStatus(string.Format("Alt+{0} 无对应启用帧", enabledIndex + 1));
                return;
            }

            var row = rows[enabledIndex];
            if (SendTxFrameRow(row))
            {
                row.LastSentUtc = DateTime.UtcNow;
                SetStatus(string.Format("快捷键单次发送成功（第{0}条启用帧）", enabledIndex + 1));
            }
        }

        private bool SendTxFrameRow(TxFrameRow row)
        {
            try
            {
                if (row == null)
                {
                    return false;
                }

                var cfg = _channelConfigs.FirstOrDefault(c => c.ChannelId == row.ChannelId);
                if (cfg == null)
                {
                    SetStatus(string.Format("发送帧目标通道 CH{0} 未配置。请先在通道管理中添加。", row.ChannelId));
                    return false;
                }
                if (!cfg.Enabled)
                {
                    SetStatus(string.Format("发送帧目标通道 CH{0} 处于关闭状态。", row.ChannelId));
                    return false;
                }

                var isCanFd = IsCanFdFrameType(cfg.FrameType);
                var id = ParseHexId(row.IdHex);
                var data = ParseHexBytes(row.DataHex ?? "");

                // 防呆：经典 CAN 最多 8 字节，CAN FD 最多 64 字节
                if (!isCanFd && data.Length > 8)
                {
                    SetStatus(string.Format("CH{0} 为 CAN，帧数据长度不能超过 8 字节。", cfg.ChannelId));
                    return false;
                }
                if (isCanFd && data.Length > 64)
                {
                    SetStatus(string.Format("CH{0} 为 CAN FD，帧数据长度不能超过 64 字节。", cfg.ChannelId));
                    return false;
                }

                byte flags = (byte)((isCanFd ? PacketFlagCanFd : 0x00) | PacketFlagTx);
                var packet = BuildPacket((byte)cfg.ChannelId, id, flags, data);

                if (TrySendPacket(packet))
                {
                    NoteTxActivity(false);
                    SetStatus(string.Format(
                        "已发送 CH={0} ID=0x{1:X3} {2} N={3} D={4}",
                        cfg.ChannelId,
                        id,
                        isCanFd ? "CAN FD" : "CAN",
                        cfg.NominalBitrate,
                        isCanFd ? cfg.DataBitrate : 0));
                    return true;
                }
                SetStatus(string.Format("发送失败 CH={0}", cfg.ChannelId));
                return false;
            }
            catch (Exception ex)
            {
                SetStatus("发送参数错误: " + ex.Message);
                return false;
            }
        }

        private static uint ParseHexId(string text)
        {
            var t = (text ?? "").Trim();
            if (t.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                t = t.Substring(2);
            }
            return uint.Parse(t, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        }

        private void ChannelGridCellBeginEdit(object sender, DataGridViewCellCancelEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0 || e.ColumnIndex < 0)
            {
                return;
            }
            if (_isConnected)
            {
                e.Cancel = true;
                SetStatus("设备已连接，通道配置已锁定。请先断开连接后再修改。");
                return;
            }

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null)
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            if (col.DataPropertyName == "NominalSamplePointText" &&
                !string.Equals(NormalizeSamplePreset(row.NominalSamplePreset, false), SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
            {
                e.Cancel = true;
                return;
            }

            if (col.DataPropertyName == "DataSamplePointText")
            {
                if (!IsCanFdFrameType(row.FrameType) ||
                    !string.Equals(NormalizeSamplePreset(row.DataSamplePreset, true), SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
                {
                    e.Cancel = true;
                }
                return;
            }

            if ((col.DataPropertyName == "DataBitrate") || (col.DataPropertyName == "DataSamplePreset"))
            {
                // CAN 模式禁止编辑数据域参数，防止误配置
                if (!IsCanFdFrameType(row.FrameType))
                {
                    e.Cancel = true;
                }
            }
        }

        private void ChannelGridCellEndEdit(object sender, DataGridViewCellEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0 || e.ColumnIndex < 0)
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null)
            {
                return;
            }

            if (IsDeferredNormalizeChannelColumn(col.DataPropertyName))
            {
                NormalizeChannelConfigRow(row);
                if (_projectReady && !_suspendDirtyTracking)
                {
                    NoteChannelConfigChanged();
                }
                if (_channelGrid.IsCurrentCellInEditMode)
                {
                    return;
                }
                _channelGrid.InvalidateRow(e.RowIndex);
            }
        }

        private void ChannelGridCellFormatting(object sender, DataGridViewCellFormattingEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0 || e.ColumnIndex < 0)
            {
                return;
            }

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null)
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            if (col.DataPropertyName == "NominalSamplePointText")
            {
                if (string.Equals(NormalizeSamplePreset(row.NominalSamplePreset, false), SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
                {
                    e.CellStyle.BackColor = Color.White;
                    e.CellStyle.ForeColor = Color.Black;
                    e.Value = NormalizeSampleText(row.NominalSamplePointText, row.NominalSamplePreset, false);
                }
                else
                {
                    e.CellStyle.BackColor = Color.Gainsboro;
                    e.CellStyle.ForeColor = Color.DimGray;
                    e.Value = "";
                }
                e.FormattingApplied = true;
                return;
            }

            if ((col.DataPropertyName == "DataBitrate") ||
                (col.DataPropertyName == "DataSamplePointText"))
            {
                if (IsCanFdFrameType(row.FrameType))
                {
                    e.CellStyle.BackColor = Color.White;
                    e.CellStyle.ForeColor = Color.Black;
                    if (col.DataPropertyName == "DataBitrate")
                    {
                        e.Value = row.DataBitrate.ToString(CultureInfo.InvariantCulture);
                    }
                    else if (col.DataPropertyName == "DataSamplePointText")
                    {
                        if (string.Equals(NormalizeSamplePreset(row.DataSamplePreset, true), SamplePresetCustom, StringComparison.OrdinalIgnoreCase))
                        {
                            e.Value = NormalizeSampleText(row.DataSamplePointText, row.DataSamplePreset, true);
                        }
                        else
                        {
                            e.CellStyle.BackColor = Color.Gainsboro;
                            e.CellStyle.ForeColor = Color.DimGray;
                            e.Value = "";
                        }
                    }
                }
                else
                {
                    // CAN 模式下数据域栏位灰态且留空，避免文字误导
                    e.CellStyle.BackColor = Color.Gainsboro;
                    e.CellStyle.ForeColor = Color.DimGray;
                    e.Value = "";
                }
                e.FormattingApplied = true;
                return;
            }

            if (col.DataPropertyName == "NominalSamplePreset")
            {
                e.Value = NormalizeSamplePreset(row.NominalSamplePreset, false);
                e.FormattingApplied = true;
                return;
            }
            if (col.DataPropertyName == "DataSamplePreset")
            {
                e.Value = IsCanFdFrameType(row.FrameType) ? NormalizeSamplePreset(row.DataSamplePreset, true) : "";
                e.FormattingApplied = true;
                return;
            }
        }

        private void ChannelGridCellValueChanged(object sender, DataGridViewCellEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0 || e.ColumnIndex < 0)
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null)
            {
                return;
            }

            if (IsImmediateCommitChannelColumn(col.DataPropertyName))
            {
                NormalizeChannelConfigRow(row);
                if (_projectReady && !_suspendDirtyTracking)
                {
                    NoteChannelConfigChanged();
                }
                _channelGrid.InvalidateRow(e.RowIndex);
            }
        }

        private void ImportDbc()
        {
            if (!EnsureProjectReady())
            {
                return;
            }
            using (var ofd = new OpenFileDialog())
            {
                ofd.Filter = "DBC Files|*.dbc|All Files|*.*";
                if (ofd.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }
                ImportDbcFromPath(ofd.FileName);
            }
        }

        private void ImportDbcFromPath(string path)
        {
            _dbcMap.Clear();
            foreach (var kv in DbcParser.Parse(path))
            {
                _dbcMap[kv.Key] = kv.Value;
            }
            _currentDbcPath = path;
            RefreshDbcView();
            SetStatus(string.Format("DBC已导入: {0} 条消息", _dbcMap.Count));
            MarkProjectDirty();
        }

        private void RefreshDbcView()
        {
            _dbcTree.Nodes.Clear();
            _allVariableKeys.Clear();

            foreach (var msg in _dbcMap.Values.OrderBy(m => m.Id))
            {
                var mNode = new TreeNode(string.Format("0x{0:X3} {1} [{2}]", msg.Id, msg.Name, msg.Dlc));
                foreach (var sig in msg.Signals)
                {
                    var varKey = string.Format("{0}.{1}", msg.Name, sig.Name);
                    mNode.Nodes.Add(new TreeNode(string.Format("{0} ({1}|{2})", sig.Name, sig.StartBit, sig.Length)));
                    _allVariableKeys.Add(varKey);
                }
                _dbcTree.Nodes.Add(mNode);
            }

            ApplyVariableFilter();
        }

        private void AddSelectedVariables()
        {
            if (_availableVarList.SelectedItem == null)
            {
                return;
            }

            var key = _availableVarList.SelectedItem.ToString();
            if (_monitoredKeys.Contains(key))
            {
                return;
            }

            _monitoredKeys.Add(key);
            _monitoredVarList.Items.Add(key, true);
            RefreshChartSeriesBySelection();
            MarkProjectDirty();
        }

        private void RemoveSelectedVariable()
        {
            if (_monitoredVarList.SelectedItem == null)
            {
                return;
            }

            var key = _monitoredVarList.SelectedItem.ToString();
            _monitoredKeys.Remove(key);
            _monitoredVarList.Items.Remove(key);

            var removeRows = _realtimeRows.Where(r => r.Variable == key).ToList();
            foreach (var r in removeRows)
            {
                _realtimeRows.Remove(r);
                var dynamicKey = string.Format("{0}|{1}", r.Variable, r.Channel);
                _realtimeMap.Remove(dynamicKey);
            }

            var removeSeries = _chart.Series.Cast<Series>()
                .Where(s => s.Name.StartsWith(key + "[", StringComparison.Ordinal))
                .ToList();
            foreach (var s in removeSeries)
            {
                _chart.Series.Remove(s);
            }
            MarkProjectDirty();
        }

        private void PollTransport()
        {
            byte[] packet;
            while (_transport.TryReceive(out packet))
            {
                _lastDeviceActivityUtc = DateTime.UtcNow;

                if (TryHandleControlPacket(packet))
                {
                    continue;
                }

                var frame = ParsePacket(packet);
                if (frame == null)
                {
                    continue;
                }

                if (frame.IsTx)
                {
                    NoteTxActivity(frame.HasError);
                }
                else
                {
                    NoteRxActivity(frame.HasError);
                }

                AppendBusRow(frame);
                DecodeAndStore(frame);
            }
        }

        private void DecodeAndStore(CanFrame frame)
        {
            if (frame.HasError)
            {
                return;
            }

            CanMessageDefinition msg;
            if (!_dbcMap.TryGetValue(frame.Id, out msg))
            {
                return;
            }

            foreach (var sig in msg.Signals)
            {
                double value;
                if (!DbcParser.TryDecodeSignal(frame.Data, sig, out value))
                {
                    continue;
                }

                var sample = new SignalSample
                {
                    Timestamp = frame.Timestamp,
                    Channel = frame.Channel,
                    Message = msg.Name,
                    Signal = sig.Name,
                    Value = value
                };
                _samples.Add(sample);
                if (_storageSearchBox != null && !string.IsNullOrWhiteSpace(_storageSearchBox.Text))
                {
                    ApplyStorageFilter();
                }
                else
                {
                    _storageRows.Add(sample);
                }

                var key = string.Format("{0}.{1}", msg.Name, sig.Name);
                if (_monitoredKeys.Contains(key))
                {
                    var channelTag = string.Format("CH{0}", frame.Channel);
                    UpdateRealtimeRow(key, channelTag, value, frame.Timestamp);
                    if (IsVariableChecked(key))
                    {
                        var seriesName = string.Format("{0}[{1}]", key, channelTag);
                        var series = EnsureSeries(seriesName);
                        if (series != null)
                        {
                            series.Points.AddXY(sample.Timestamp.ToOADate(), sample.Value);
                            if (series.Points.Count > 500)
                            {
                                series.Points.RemoveAt(0);
                            }
                        }
                    }
                }
            }

            _chart.ChartAreas[0].RecalculateAxesScale();
        }

        private void UpdateRealtimeRow(string key, string channel, double value, DateTime ts)
        {
            var dynamicKey = string.Format("{0}|{1}", key, channel);
            RealtimeVariableRow row;
            if (!_realtimeMap.TryGetValue(dynamicKey, out row))
            {
                row = new RealtimeVariableRow
                {
                    Variable = key,
                    Channel = channel,
                    Value = "-",
                    Timestamp = "-"
                };
                _realtimeMap[dynamicKey] = row;
                _realtimeRows.Add(row);
            }

            row.Value = value.ToString("F3", CultureInfo.InvariantCulture);

            var fmt = "HH:mm:ss.fff";
            if (_timestampFormatBox != null && _timestampFormatBox.SelectedItem != null)
            {
                fmt = _timestampFormatBox.SelectedItem.ToString();
            }
            row.Timestamp = ts.ToString(fmt, CultureInfo.InvariantCulture);

            var idx = _realtimeRows.IndexOf(row);
            if (idx >= 0)
            {
                _realtimeRows.ResetItem(idx);
            }
        }

        private bool IsVariableChecked(string key)
        {
            if (_monitoredVarList == null)
            {
                return false;
            }

            var idx = _monitoredVarList.Items.IndexOf(key);
            if (idx < 0)
            {
                return false;
            }

            return _monitoredVarList.GetItemChecked(idx);
        }

        private Series EnsureSeries(string seriesName)
        {
            var existing = _chart.Series.FindByName(seriesName);
            if (existing != null)
            {
                return existing;
            }

            var s = new Series(seriesName);
            s.ChartType = SeriesChartType.Line;
            s.BorderWidth = 2;
            s.XValueType = ChartValueType.DateTime;
            _chart.Series.Add(s);
            return s;
        }

        private void RefreshChartSeriesBySelection()
        {
            if (_monitoredVarList == null || _chart == null)
            {
                return;
            }

            var checkedKeys = new HashSet<string>();
            foreach (var item in _monitoredVarList.CheckedItems)
            {
                checkedKeys.Add(item.ToString());
            }

            var removeSeries = _chart.Series.Cast<Series>()
                .Where(s => !checkedKeys.Any(k => s.Name.StartsWith(k + "[", StringComparison.Ordinal)))
                .ToList();
            foreach (var s in removeSeries)
            {
                _chart.Series.Remove(s);
            }
        }

        private void ApplyVariableFilter()
        {
            if (_availableVarList == null)
            {
                return;
            }

            var old = _availableVarList.SelectedItem == null ? null : _availableVarList.SelectedItem.ToString();
            var q = _findVariableBox == null ? "" : (_findVariableBox.Text ?? "").Trim();
            var field = _findVariableFieldBox == null || _findVariableFieldBox.SelectedItem == null ? "全部" : _findVariableFieldBox.SelectedItem.ToString();
            _availableVarList.Items.Clear();

            IEnumerable<string> rows = _allVariableKeys;
            if (q.Length > 0)
            {
                rows = rows.Where(s =>
                {
                    var msg = s;
                    var sig = s;
                    var dot = s.IndexOf('.');
                    if (dot > 0)
                    {
                        msg = s.Substring(0, dot);
                        sig = s.Substring(dot + 1);
                    }
                    if (field == "消息名")
                    {
                        return msg.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    if (field == "信号名")
                    {
                        return sig.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    return s.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
                });
            }

            foreach (var key in rows)
            {
                _availableVarList.Items.Add(key);
            }

            if (old != null)
            {
                var idx = _availableVarList.Items.IndexOf(old);
                if (idx >= 0)
                {
                    _availableVarList.SelectedIndex = idx;
                }
            }

            if (_availableVarList.Items.Count == 0)
            {
                SetStatus("筛选后无匹配变量");
            }
        }

        private void ApplyStorageFilter()
        {
            if (_storageRows == null)
            {
                return;
            }

            var q = _storageSearchBox == null ? "" : (_storageSearchBox.Text ?? "").Trim();
            var field = _storageSearchFieldBox == null || _storageSearchFieldBox.SelectedItem == null ? "全部" : _storageSearchFieldBox.SelectedItem.ToString();
            _storageRows.RaiseListChangedEvents = false;
            _storageRows.Clear();
            for (int i = 0; i < _samples.Count; i++)
            {
                var s = _samples[i];
                if (q.Length == 0 || MatchStorageSearch(s, field, q))
                {
                    _storageRows.Add(s);
                }
            }
            _storageRows.RaiseListChangedEvents = true;
            _storageRows.ResetBindings();
        }

        private void NewProject()
        {
            if (!TryCloseCurrentProjectWithPrompt())
            {
                return;
            }
            NewProjectInternal();
        }

        private void NewProjectInternal()
        {
            _suspendDirtyTracking = true;
            SetProjectReady(true);
            _currentProjectPath = null;
            _currentDbcPath = null;
            _busLogPath = null;
            _addressBox.Text = "mock://localhost";
            _dbcMap.Clear();
            _allVariableKeys.Clear();
            _txFrames.Clear();
            _channelConfigs.Clear();
            _realtimeRows.Clear();
            _realtimeMap.Clear();
            _monitoredKeys.Clear();
            _monitoredVarList.Items.Clear();
            _busRows.Clear();
            _busViewRows.Clear();
            _chart.Series.Clear();
            _samples.Clear();
            _storageRows.Clear();
            RefreshDbcView();
            MarkBusViewDirty(true);
            _suspendDirtyTracking = false;
            SetProjectDirty(false);
            ResetTrafficIndicators();
            UpdateActionStates();
            SetStatus("已新建空工程");
        }

        private void OpenProject()
        {
            using (var ofd = new OpenFileDialog())
            {
                ofd.Filter = "EverVance Project|*.evproj|All Files|*.*";
                if (ofd.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }
                if (_projectReady && PathsEqual(ofd.FileName, _currentProjectPath))
                {
                    SetStatus("当前工程已打开。");
                    return;
                }
                if (!TryCloseCurrentProjectWithPrompt())
                {
                    return;
                }
                var dir = Path.GetDirectoryName(ofd.FileName);
                if (!string.IsNullOrWhiteSpace(dir))
                {
                    SetWorkspacePath(dir);
                    SaveLastWorkspacePath(dir);
                    RefreshWorkspaceProjects();
                }
                LoadProjectFromFile(ofd.FileName);
                SelectWorkspaceProject(ofd.FileName);
            }
        }

        private void SaveProject()
        {
            SaveProjectWithResult(false);
        }

        private void SaveProjectAs()
        {
            SaveProjectWithResult(true);
        }

        private bool SaveProjectWithResult(bool forceSaveAs)
        {
            if (forceSaveAs || string.IsNullOrEmpty(_currentProjectPath))
            {
                using (var sfd = new SaveFileDialog())
                {
                    sfd.Filter = "EverVance Project|*.evproj|All Files|*.*";
                    sfd.FileName = "evervance.evproj";
                    if (sfd.ShowDialog(this) != DialogResult.OK)
                    {
                        return false;
                    }
                    _currentProjectPath = sfd.FileName;
                    var dir = Path.GetDirectoryName(_currentProjectPath);
                    if (!string.IsNullOrWhiteSpace(dir))
                    {
                        SetWorkspacePath(dir);
                        SaveLastWorkspacePath(dir);
                        RefreshWorkspaceProjects();
                    }
                    SelectWorkspaceProject(_currentProjectPath);
                }
            }

            SaveProjectToFile(_currentProjectPath);
            return true;
        }

        private void SaveProjectToFile(string path)
        {
            var lines = new List<string>();
            lines.Add("[Project]");
            lines.Add("Address=" + EncodeProjectValue(_addressBox == null ? "" : _addressBox.Text));
            lines.Add("DbcPath=" + EncodeProjectValue(_currentDbcPath ?? ""));
            lines.Add("");
            lines.Add("[Channels]");
            foreach (var c in _channelConfigs.OrderBy(c => c.ChannelId))
            {
                NormalizeChannelConfigRow(c);
                lines.Add(string.Format("{0}\t{1}\t{2}\t{3}\t{4}\t{5}\t{6}\t{7}\t{8}\t{9}",
                    c.ChannelId,
                    c.Enabled ? 1 : 0,
                    EncodeProjectValue(c.FrameType ?? FrameTypeClassic),
                    c.NominalBitrate,
                    c.DataBitrate,
                    EncodeProjectValue(c.NominalSamplePreset ?? "80.0%"),
                    EncodeProjectValue(c.NominalSamplePointText ?? "80.0"),
                    EncodeProjectValue(c.DataSamplePreset ?? "75.0%"),
                    EncodeProjectValue(c.DataSamplePointText ?? "75.0"),
                    c.TerminationEnabled ? 1 : 0));
            }
            lines.Add("");
            lines.Add("[TxFrames]");
            foreach (var f in _txFrames)
            {
                lines.Add(string.Format("{0}\t{1}\t{2}\t{3}\t{4}\t{5}",
                    f.Enabled ? 1 : 0,
                    f.ChannelId,
                    EncodeProjectValue(f.IdHex ?? ""),
                    EncodeProjectValue(f.DataHex ?? ""),
                    f.IntervalMs,
                    EncodeProjectValue(f.Note ?? "")));
            }
            lines.Add("");
            lines.Add("[Monitored]");
            if (_monitoredVarList != null)
            {
                for (int i = 0; i < _monitoredVarList.Items.Count; i++)
                {
                    var key = _monitoredVarList.Items[i].ToString();
                    var ck = _monitoredVarList.GetItemChecked(i) ? "1" : "0";
                    lines.Add(EncodeProjectValue(key) + "\t" + ck);
                }
            }
            lines.Add("");
            lines.Add("[Layout]");
            lines.Add("WindowState=" + (WindowState == FormWindowState.Maximized ? "Maximized" : "Normal"));
            lines.Add("WindowX=" + Left.ToString(CultureInfo.InvariantCulture));
            lines.Add("WindowY=" + Top.ToString(CultureInfo.InvariantCulture));
            lines.Add("WindowW=" + Width.ToString(CultureInfo.InvariantCulture));
            lines.Add("WindowH=" + Height.ToString(CultureInfo.InvariantCulture));
            if (_mainSplit != null)
            {
                lines.Add("MainSplit=" + _mainSplit.SplitterDistance.ToString(CultureInfo.InvariantCulture));
            }
            if (_plotSplit != null)
            {
                lines.Add("PlotSplit=" + _plotSplit.SplitterDistance.ToString(CultureInfo.InvariantCulture));
            }

            File.WriteAllLines(path, lines.ToArray(), Encoding.UTF8);
            _busLogPath = Path.ChangeExtension(path, ".buslog.csv");
            SetProjectDirty(false);
            UpdateActionStates();
            SetStatus("工程已保存: " + path);
        }

        private void LoadProjectFromFile(string path)
        {
            if (!File.Exists(path))
            {
                MessageBox.Show("工程文件不存在。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            NewProjectInternal();
            _currentProjectPath = path;
            _busLogPath = Path.ChangeExtension(path, ".buslog.csv");
            _suspendDirtyTracking = true;

            string section = "";
            var monitoredLoad = new List<KeyValuePair<string, bool>>();
            var layoutMap = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                var line = raw.Trim();
                if (line.Length == 0)
                {
                    continue;
                }
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    section = line;
                    continue;
                }

                if (section == "[Project]")
                {
                    var idx = line.IndexOf('=');
                    if (idx <= 0) continue;
                    var key = line.Substring(0, idx);
                    var val = DecodeProjectValue(line.Substring(idx + 1));
                    if (key == "Address")
                    {
                        _addressBox.Text = val;
                    }
                    else if (key == "DbcPath")
                    {
                        _currentDbcPath = val;
                    }
                }
                else if (section == "[Channels]")
                {
                    var p = line.Split('\t');
                    if (p.Length < 5) continue;
                    var row = new ChannelConfigRow
                    {
                        ChannelId = ParseIntSafe(p[0], 0),
                        Enabled = ParseIntSafe(p[1], 1) != 0,
                        FrameType = DecodeProjectValue(p[2]),
                        NominalBitrate = ParseIntSafe(p[3], 500000),
                        DataBitrate = ParseIntSafe(p[4], 0),
                        NominalSamplePreset = p.Length > 5 ? DecodeProjectValue(p[5]) : "80.0%",
                        NominalSamplePointText = p.Length > 6 ? DecodeProjectValue(p[6]) : "80.0",
                        DataSamplePreset = p.Length > 7 ? DecodeProjectValue(p[7]) : "75.0%",
                        DataSamplePointText = p.Length > 8 ? DecodeProjectValue(p[8]) : "75.0",
                        TerminationEnabled = p.Length > 9 && ParseIntSafe(p[9], 0) != 0
                    };
                    NormalizeChannelConfigRow(row);
                    _channelConfigs.Add(row);
                }
                else if (section == "[TxFrames]")
                {
                    var p = line.Split('\t');
                    if (p.Length < 6) continue;
                    _txFrames.Add(new TxFrameRow
                    {
                        Enabled = ParseIntSafe(p[0], 1) != 0,
                        ChannelId = ParseIntSafe(p[1], 0),
                        IdHex = DecodeProjectValue(p[2]),
                        DataHex = DecodeProjectValue(p[3]),
                        IntervalMs = ParseIntSafe(p[4], 100),
                        Note = DecodeProjectValue(p[5]),
                        LastSentUtc = DateTime.MinValue
                    });
                }
                else if (section == "[Monitored]")
                {
                    var p = line.Split('\t');
                    if (p.Length < 2) continue;
                    monitoredLoad.Add(new KeyValuePair<string, bool>(DecodeProjectValue(p[0]), ParseIntSafe(p[1], 1) != 0));
                }
                else if (section == "[Layout]")
                {
                    var idx = line.IndexOf('=');
                    if (idx <= 0) continue;
                    layoutMap[line.Substring(0, idx)] = line.Substring(idx + 1);
                }
            }

            if (!string.IsNullOrWhiteSpace(_currentDbcPath) && File.Exists(_currentDbcPath))
            {
                ImportDbcFromPath(_currentDbcPath);
            }

            SortChannelConfigs();
            MarkBusViewDirty(true);

            foreach (var kv in monitoredLoad)
            {
                if (_allVariableKeys.Contains(kv.Key))
                {
                    _monitoredKeys.Add(kv.Key);
                    _monitoredVarList.Items.Add(kv.Key, kv.Value);
                }
            }
            RefreshChartSeriesBySelection();

            SetStatus("工程已加载: " + path);
            SetProjectReady(true);
            LoadBusLog();
            MarkBusViewDirty(true);
            ApplyLayoutFromProject(layoutMap);
            _suspendDirtyTracking = false;
            SetProjectDirty(false);
            UpdateActionStates();
        }

        private void SelectWorkspaceProject(string path)
        {
            if (_workspaceProjectList == null || string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            var idx = _workspaceProjectList.Items.IndexOf(path);
            if (idx >= 0)
            {
                _workspaceProjectList.SelectedIndex = idx;
            }
        }

        private void MainForm_FormClosing(object sender, FormClosingEventArgs e)
        {
            if (!TryCloseCurrentProjectWithPrompt())
            {
                e.Cancel = true;
                return;
            }
            if (_appState == null)
            {
                _appState = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            }
            _appState["WorkspacePath"] = string.IsNullOrWhiteSpace(_workspacePath) ? Environment.CurrentDirectory : _workspacePath;
            _appState["WindowX"] = Left.ToString(CultureInfo.InvariantCulture);
            _appState["WindowY"] = Top.ToString(CultureInfo.InvariantCulture);
            _appState["WindowW"] = Width.ToString(CultureInfo.InvariantCulture);
            _appState["WindowH"] = Height.ToString(CultureInfo.InvariantCulture);
            if (_mainSplit != null)
            {
                _appState["MainSplit"] = _mainSplit.SplitterDistance.ToString(CultureInfo.InvariantCulture);
            }
            if (_plotSplit != null)
            {
                _appState["PlotSplit"] = _plotSplit.SplitterDistance.ToString(CultureInfo.InvariantCulture);
            }
            SaveAppState();
        }

        private void ResetTrafficIndicators()
        {
            _rxFlashUntilUtc = DateTime.MinValue;
            _txFlashUntilUtc = DateTime.MinValue;
            _rxErrorLatched = false;
            _txErrorLatched = false;
            RenderIndicators();
        }

        private void NoteRxActivity(bool hasError)
        {
            _rxFlashUntilUtc = DateTime.UtcNow.AddMilliseconds(220);
            if (hasError)
            {
                _rxErrorLatched = true;
            }
            RenderIndicators();
        }

        private void NoteTxActivity(bool hasError)
        {
            _txFlashUntilUtc = DateTime.UtcNow.AddMilliseconds(220);
            if (hasError)
            {
                _txErrorLatched = true;
            }
            RenderIndicators();
        }

        private static void SetLed(ToolStripStatusLabel led, bool on, Color onColor)
        {
            if (led == null)
            {
                return;
            }
            led.Text = "●";
            led.ForeColor = on ? onColor : Color.DarkGray;
        }

        private void RenderIndicators()
        {
            var now = DateTime.UtcNow;
            SetLed(_ledProject, _projectReady, Color.LimeGreen);
            SetLed(_ledConnect, _isConnected, Color.LimeGreen);
            SetLed(_ledRxFlow, now <= _rxFlashUntilUtc, Color.LimeGreen);
            SetLed(_ledTxFlow, now <= _txFlashUntilUtc, Color.LimeGreen);
            SetLed(_ledRxError, _rxErrorLatched, Color.Red);
            SetLed(_ledTxError, _txErrorLatched, Color.Red);
        }

        private static int ParseIntSafe(string s, int dft)
        {
            int v;
            return int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : dft;
        }

        private static string EncodeProjectValue(string s)
        {
            var text = s ?? "";
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(text));
        }

        private static string DecodeProjectValue(string s)
        {
            try
            {
                var buf = Convert.FromBase64String(s ?? "");
                return Encoding.UTF8.GetString(buf);
            }
            catch
            {
                return "";
            }
        }

        private void ApplyLayoutFromProject(Dictionary<string, string> map)
        {
            if (map == null || map.Count == 0)
            {
                return;
            }

            int x = ParseIntSafe(GetMap(map, "WindowX"), Left);
            int y = ParseIntSafe(GetMap(map, "WindowY"), Top);
            int w = ParseIntSafe(GetMap(map, "WindowW"), Width);
            int h = ParseIntSafe(GetMap(map, "WindowH"), Height);
            if (w >= MinimumSize.Width && h >= MinimumSize.Height)
            {
                StartPosition = FormStartPosition.Manual;
                Left = x;
                Top = y;
                Width = w;
                Height = h;
            }

            if (string.Equals(GetMap(map, "WindowState"), "Maximized", StringComparison.OrdinalIgnoreCase))
            {
                WindowState = FormWindowState.Maximized;
            }

            if (_mainSplit != null)
            {
                var d = ParseIntSafe(GetMap(map, "MainSplit"), _mainSplit.SplitterDistance);
                var min = Math.Max(_mainSplit.Panel1MinSize, 280);
                var max = Math.Max(min, _mainSplit.Width - 520);
                _mainSplit.SplitterDistance = Math.Max(min, Math.Min(max, d));
            }
            if (_plotSplit != null)
            {
                var d = ParseIntSafe(GetMap(map, "PlotSplit"), _plotSplit.SplitterDistance);
                var min = 320;
                var max = Math.Max(min, _plotSplit.Width - 320);
                _plotSplit.SplitterDistance = Math.Max(min, Math.Min(max, d));
            }
        }

        private static string GetMap(Dictionary<string, string> map, string key)
        {
            string value;
            return map.TryGetValue(key, out value) ? value : "";
        }

        private void AppendBusLog(BusFrameRow row)
        {
            if (row == null || string.IsNullOrWhiteSpace(_busLogPath))
            {
                return;
            }

            try
            {
                var needHeader = !File.Exists(_busLogPath);
                using (var sw = new StreamWriter(_busLogPath, true, Encoding.UTF8))
                {
                    if (needHeader)
                    {
                        sw.WriteLine("SortTime,Timestamp,Channel,Direction,FrameType,IdHex,Dlc,DataHex,ErrorState,ErrorType");
                    }
                    sw.WriteLine(string.Format("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}",
                        row.SortTime.ToString("O"),
                        Csv(row.Timestamp),
                        row.Channel,
                        Csv(row.Direction),
                        Csv(row.FrameType),
                        Csv(row.IdHex),
                        row.Dlc,
                        Csv(row.DataHex),
                        Csv(row.ErrorState),
                        Csv(row.ErrorType)));
                }
            }
            catch
            {
            }
        }

        private void LoadBusLog()
        {
            if (string.IsNullOrWhiteSpace(_busLogPath) || !File.Exists(_busLogPath))
            {
                return;
            }

            try
            {
                var lines = File.ReadAllLines(_busLogPath, Encoding.UTF8);
                if (lines.Length <= 1)
                {
                    return;
                }

                _busRows.Clear();
                for (int i = 1; i < lines.Length; i++)
                {
                    var p = lines[i].Split(',');
                    if (p.Length < 10)
                    {
                        continue;
                    }

                    DateTime st;
                    if (!DateTime.TryParse(p[0], null, DateTimeStyles.RoundtripKind, out st))
                    {
                        st = DateTime.Now;
                    }

                    _busRows.Add(new BusFrameRow
                    {
                        SortTime = st,
                        Timestamp = p[1],
                        Channel = ParseIntSafe(p[2], 0),
                        Direction = p[3],
                        FrameType = p[4],
                        IdHex = p[5],
                        Dlc = ParseIntSafe(p[6], 0),
                        DataHex = p[7],
                        ErrorState = p[8],
                        ErrorType = p[9]
                    });
                }
            }
            catch
            {
            }
        }

        private static string Csv(string text)
        {
            var s = text ?? "";
            return s.Replace(',', ' ').Replace('\r', ' ').Replace('\n', ' ').Trim();
        }

        private void AppendBusRow(CanFrame frame)
        {
            var row = new BusFrameRow
            {
                SortTime = frame.Timestamp,
                Timestamp = frame.Timestamp.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture),
                Channel = frame.Channel,
                Direction = frame.IsTx ? "TX" : "RX",
                FrameType = frame.IsCanFd ? "CAN FD" : "CAN",
                IdHex = string.Format("0x{0:X3}", frame.Id),
                Dlc = frame.Data == null ? 0 : frame.Data.Length,
                DataHex = frame.Data == null ? "" : BitConverter.ToString(frame.Data).Replace("-", " "),
                ErrorState = frame.HasError ? "ERROR" : "OK",
                ErrorType = frame.HasError ? frame.ErrorType : ""
            };

            _busRows.Add(row);
            if (_busRows.Count > 2000)
            {
                _busRows.RemoveAt(0);
            }
            AppendBusLog(row);
            MarkBusViewDirty(false);
        }

        private void MarkBusViewDirty(bool refreshNow)
        {
            _busViewDirty = true;
            if (refreshNow)
            {
                _busViewDirty = false;
                RefreshBusMonitorView();
            }
        }

        private void RefreshBusMonitorView()
        {
            if (_busGrid == null)
            {
                return;
            }

            var enabledChannels = new HashSet<int>();
            if (_busChannelFilter != null)
            {
                for (int i = 0; i < _busChannelFilter.Items.Count; i++)
                {
                    if (_busChannelFilter.GetItemChecked(i))
                    {
                        enabledChannels.Add(i);
                    }
                }
            }

            IEnumerable<BusFrameRow> rows = _busRows;
            if (_busChannelFilter != null && enabledChannels.Count == 0)
            {
                rows = Enumerable.Empty<BusFrameRow>();
            }
            else if (enabledChannels.Count > 0)
            {
                rows = rows.Where(r => enabledChannels.Contains(r.Channel));
            }

            if (_busOnlyErrorBox != null && _busOnlyErrorBox.Checked)
            {
                rows = rows.Where(r => r.ErrorState == "ERROR");
            }

            var q = _busSearchBox == null ? "" : (_busSearchBox.Text ?? "").Trim();
            var searchField = _busSearchFieldBox == null || _busSearchFieldBox.SelectedItem == null ? "全部" : _busSearchFieldBox.SelectedItem.ToString();
            if (q.Length > 0)
            {
                rows = rows.Where(r => MatchBusSearch(r, searchField, q));
            }

            var sortMode = _busSortBox == null || _busSortBox.SelectedItem == null ? "时间降序" : _busSortBox.SelectedItem.ToString();
            if (sortMode == "时间升序")
            {
                rows = rows.OrderBy(r => r.SortTime);
            }
            else if (sortMode == "ID升序")
            {
                rows = rows.OrderBy(r => r.IdHex).ThenBy(r => r.SortTime);
            }
            else if (sortMode == "ID降序")
            {
                rows = rows.OrderByDescending(r => r.IdHex).ThenByDescending(r => r.SortTime);
            }
            else
            {
                rows = rows.OrderByDescending(r => r.SortTime);
            }

            var list = rows.ToList();
            _busViewRows.RaiseListChangedEvents = false;
            _busViewRows.Clear();
            for (int i = 0; i < list.Count; i++)
            {
                _busViewRows.Add(list[i]);
            }
            _busViewRows.RaiseListChangedEvents = true;
            _busViewRows.ResetBindings();
        }

        private void ClearBusMonitorRows()
        {
            _busRows.Clear();
            _busViewRows.Clear();
            MarkBusViewDirty(true);
            SetStatus("总线监控日志已清空。");
        }

        private static bool MatchStorageSearch(SignalSample s, string field, string q)
        {
            var ts = s.Timestamp.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture);
            var ch = s.Channel.ToString(CultureInfo.InvariantCulture);
            var msg = s.Message ?? "";
            var sig = s.Signal ?? "";
            var val = s.Value.ToString(CultureInfo.InvariantCulture);

            if (field == "时间戳") return ts.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "通道") return ch.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "消息名") return msg.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "信号名") return sig.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "数值") return val.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            return ts.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   ch.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   msg.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   sig.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   val.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static bool MatchBusSearch(BusFrameRow r, string field, string q)
        {
            var ch = string.Format("CH{0}", r.Channel);
            if (field == "时间") return r.Timestamp.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "方向") return r.Direction.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "类型") return r.FrameType.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "ID") return r.IdHex.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "DLC") return r.Dlc.ToString(CultureInfo.InvariantCulture).IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "数据") return r.DataHex.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "状态") return r.ErrorState.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "错误类型") return r.ErrorType.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            if (field == "通道") return ch.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
            return r.IdHex.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   r.DataHex.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   r.ErrorType.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   r.Direction.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   r.FrameType.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   r.Timestamp.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0 ||
                   ch.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private void BusGridCellFormatting(object sender, DataGridViewCellFormattingEventArgs e)
        {
            if (_busGrid == null || e.RowIndex < 0)
            {
                return;
            }

            var row = _busGrid.Rows[e.RowIndex].DataBoundItem as BusFrameRow;
            if (row == null)
            {
                return;
            }

            if (row.ErrorState == "ERROR")
            {
                e.CellStyle.ForeColor = Color.Red;
                e.CellStyle.BackColor = Color.LightYellow;
            }
        }

        private void ExportCsv()
        {
            using (var sfd = new SaveFileDialog())
            {
                sfd.Filter = "CSV|*.csv";
                sfd.FileName = "evervance_samples.csv";
                if (sfd.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }

                using (var sw = new StreamWriter(sfd.FileName))
                {
                    sw.WriteLine("Timestamp,Channel,Message,Signal,Value");
                    foreach (var s in _samples)
                    {
                        sw.WriteLine(string.Format("{0},{1},{2},{3},{4}",
                            s.Timestamp.ToString("O"),
                            s.Channel,
                            s.Message,
                            s.Signal,
                            s.Value.ToString(CultureInfo.InvariantCulture)));
                    }
                }
            }

            _status.Text = "CSV导出完成";
        }

        private void ExportStorageLog()
        {
            using (var sfd = new SaveFileDialog())
            {
                sfd.Filter = "CSV|*.csv";
                sfd.FileName = "evervance_storage_log.csv";
                if (sfd.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }

                using (var sw = new StreamWriter(sfd.FileName, false, Encoding.UTF8))
                {
                    sw.WriteLine("Timestamp,Channel,Message,Signal,Value");
                    foreach (var s in _samples)
                    {
                        sw.WriteLine(string.Format("{0},{1},{2},{3},{4}",
                            s.Timestamp.ToString("O"),
                            s.Channel,
                            Csv(s.Message),
                            Csv(s.Signal),
                            s.Value.ToString(CultureInfo.InvariantCulture)));
                    }
                }
            }

            SetStatus(string.Format("变量存储日志已导出，共 {0} 条。", _samples.Count));
        }

        private void ImportStorageLog()
        {
            using (var ofd = new OpenFileDialog())
            {
                ofd.Filter = "CSV|*.csv|All Files|*.*";
                if (ofd.ShowDialog(this) != DialogResult.OK)
                {
                    return;
                }

                int imported = 0;
                bool headerDecided = false;
                int tsIdx = 0;
                int chIdx = 1;
                int msgIdx = 2;
                int sigIdx = 3;
                int valIdx = 4;

                foreach (var raw in File.ReadAllLines(ofd.FileName, Encoding.UTF8))
                {
                    var line = raw ?? "";
                    if (line.Trim().Length == 0)
                    {
                        continue;
                    }

                    var p = ParseCsvLine(line);
                    if (p.Count == 0)
                    {
                        continue;
                    }

                    if (!headerDecided)
                    {
                        headerDecided = true;
                        if (LooksLikeHeaderRow(p))
                        {
                            tsIdx = FindHeaderIndex(p, "timestamp", "time", "datetime", "date");
                            chIdx = FindHeaderIndex(p, "channel", "ch", "bus");
                            msgIdx = FindHeaderIndex(p, "message", "msg", "id", "idhex", "frameid");
                            sigIdx = FindHeaderIndex(p, "signal", "sig", "direction", "dir");
                            valIdx = FindHeaderIndex(p, "value", "val", "dlc", "data");
                            continue;
                        }
                    }

                    var tsText = GetCsvField(p, tsIdx);
                    var chText = GetCsvField(p, chIdx);
                    var msgText = GetCsvField(p, msgIdx);
                    var sigText = GetCsvField(p, sigIdx);
                    var valText = GetCsvField(p, valIdx);

                    if (string.IsNullOrWhiteSpace(tsText) && string.IsNullOrWhiteSpace(msgText) && string.IsNullOrWhiteSpace(sigText))
                    {
                        continue;
                    }

                    DateTime ts;
                    if (!DateTime.TryParse(tsText, null, DateTimeStyles.RoundtripKind, out ts) &&
                        !DateTime.TryParse(tsText, out ts))
                    {
                        ts = DateTime.Now;
                    }

                    int ch;
                    if (!int.TryParse(chText, NumberStyles.Integer, CultureInfo.InvariantCulture, out ch))
                    {
                        ch = 0;
                    }

                    double v;
                    if (!double.TryParse(valText, NumberStyles.Float, CultureInfo.InvariantCulture, out v) &&
                        !double.TryParse(valText, NumberStyles.Float, CultureInfo.CurrentCulture, out v))
                    {
                        v = 0;
                    }

                    _samples.Add(new SignalSample
                    {
                        Timestamp = ts,
                        Channel = ch,
                        Message = string.IsNullOrWhiteSpace(msgText) ? "Imported" : msgText,
                        Signal = string.IsNullOrWhiteSpace(sigText) ? "Value" : sigText,
                        Value = v
                    });
                    imported++;
                }

                ApplyStorageFilter();
                SetStatus(string.Format("变量存储日志已导入: {0} 条", imported));
            }
        }

        private static bool LooksLikeHeaderRow(List<string> fields)
        {
            if (fields == null || fields.Count == 0)
            {
                return false;
            }
            var joined = string.Join(",", fields).ToLowerInvariant();
            return joined.Contains("timestamp") ||
                   joined.Contains("time") ||
                   joined.Contains("channel") ||
                   joined.Contains("message") ||
                   joined.Contains("signal") ||
                   joined.Contains("value") ||
                   joined.Contains("id");
        }

        private static int FindHeaderIndex(List<string> fields, params string[] names)
        {
            for (int i = 0; i < fields.Count; i++)
            {
                var key = (fields[i] ?? "").Trim().ToLowerInvariant();
                for (int j = 0; j < names.Length; j++)
                {
                    if (key == names[j])
                    {
                        return i;
                    }
                }
            }
            return -1;
        }

        private static string GetCsvField(List<string> fields, int idx)
        {
            if (fields == null || idx < 0 || idx >= fields.Count)
            {
                return "";
            }
            return (fields[idx] ?? "").Trim();
        }

        private static List<string> ParseCsvLine(string line)
        {
            var result = new List<string>();
            if (line == null)
            {
                return result;
            }

            var sb = new StringBuilder();
            bool inQuote = false;
            for (int i = 0; i < line.Length; i++)
            {
                var ch = line[i];
                if (ch == '"')
                {
                    if (inQuote && i + 1 < line.Length && line[i + 1] == '"')
                    {
                        sb.Append('"');
                        i++;
                    }
                    else
                    {
                        inQuote = !inQuote;
                    }
                    continue;
                }

                if (ch == ',' && !inQuote)
                {
                    result.Add(sb.ToString());
                    sb.Clear();
                    continue;
                }
                sb.Append(ch);
            }
            result.Add(sb.ToString());
            return result;
        }

        private static byte[] ParseHexBytes(string input)
        {
            var parts = input.Split(new[] { ' ', ',', ';', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            var data = new List<byte>(parts.Length);
            foreach (var p in parts)
            {
                data.Add(byte.Parse(p, NumberStyles.HexNumber, CultureInfo.InvariantCulture));
            }
            return data.ToArray();
        }

        private static byte[] BuildPacket(byte channel, uint id, byte flags, byte[] data)
        {
            var len = 8 + data.Length;
            var buf = new byte[len];
            buf[0] = PacketSync;
            buf[1] = channel;
            buf[2] = (byte)data.Length;
            buf[3] = flags;
            buf[4] = (byte)(id & 0xFF);
            buf[5] = (byte)((id >> 8) & 0xFF);
            buf[6] = (byte)((id >> 16) & 0xFF);
            buf[7] = (byte)((id >> 24) & 0xFF);
            Buffer.BlockCopy(data, 0, buf, 8, data.Length);
            return buf;
        }

        private bool TryHandleControlPacket(byte[] packet)
        {
            byte channel;
            byte command;
            byte status;
            byte sequence;
            byte[] payload;

            if (!TryParseControlPacket(packet, out channel, out command, out status, out sequence, out payload))
            {
                return false;
            }

            if (command == ProtocolCmdSetChannelConfig)
            {
                HandleChannelConfigAck(channel, status, payload);
                return true;
            }
            if (command == ProtocolCmdGetChannelConfig)
            {
                HandleChannelConfigAck(channel, status, payload);
                return true;
            }
            if (command == ProtocolCmdGetDeviceInfo)
            {
                HandleDeviceInfoAck(status, payload);
                return true;
            }
            if (command == ProtocolCmdGetChannelCapabilities)
            {
                HandleChannelCapabilitiesAck(channel, status, payload);
                return true;
            }
            if (command == ProtocolCmdGetRuntimeStatus)
            {
                HandleRuntimeStatusAck(channel, status, payload);
                return true;
            }
            if (command == ProtocolCmdHeartbeat)
            {
                HandleHeartbeatAck(status, payload);
                return true;
            }

            SetStatus(string.Format(CultureInfo.InvariantCulture, "收到未知控制包: CMD=0x{0:X2}, CH{1}, SEQ={2}", command, channel, sequence));
            return true;
        }

        private static bool TryParseControlPacket(byte[] packet, out byte channel, out byte command, out byte status, out byte sequence, out byte[] payload)
        {
            channel = 0;
            command = 0;
            status = 0;
            sequence = 0;
            payload = null;

            if (packet == null || packet.Length < 8 || packet[0] != PacketSync || (packet[3] & PacketFlagControl) == 0 || packet[7] != ProtocolVersion)
            {
                return false;
            }

            var length = packet[2];
            if (packet.Length < 8 + length)
            {
                return false;
            }

            channel = packet[1];
            command = packet[4];
            status = packet[5];
            sequence = packet[6];
            payload = new byte[length];
            if (length > 0)
            {
                Buffer.BlockCopy(packet, 8, payload, 0, length);
            }
            return true;
        }

        private void HandleChannelConfigAck(byte channel, byte status, byte[] payload)
        {
            ChannelConfigRow row;
            if (payload == null || payload.Length < 16 || payload[0] != ProtocolVersion)
            {
                SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置回执格式无效。", channel));
                return;
            }

            row = _channelConfigs.FirstOrDefault(c => c.ChannelId == channel);
            if (row != null)
            {
                _suspendDirtyTracking = true;
                row.FrameType = payload[1] != 0 ? FrameTypeFd : FrameTypeClassic;
                row.Enabled = payload[2] != 0;
                row.TerminationEnabled = payload[3] != 0;
                row.NominalBitrate = (int)ReadUInt32LittleEndian(payload, 4);
                row.NominalSamplePointText = (ReadUInt16LittleEndian(payload, 8) / 10.0).ToString("0.0", CultureInfo.InvariantCulture);
                row.DataBitrate = (int)ReadUInt32LittleEndian(payload, 10);
                row.DataSamplePointText = (ReadUInt16LittleEndian(payload, 14) / 10.0).ToString("0.0", CultureInfo.InvariantCulture);
                row.NominalSamplePreset = SamplePresetCustom;
                row.DataSamplePreset = SamplePresetCustom;
                NormalizeChannelConfigRow(row);
                _suspendDirtyTracking = false;
                if (_channelGrid != null)
                {
                    _channelGrid.Invalidate();
                }
            }

            switch (status)
            {
                case ProtocolStatusOk:
                    SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置已生效。", channel));
                    break;
                case ProtocolStatusStagedOnly:
                    SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置已缓存，并已应用使能/终端；位时序待底层驱动接入。", channel));
                    break;
                case ProtocolStatusInvalid:
                    SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置被设备拒绝，请检查波特率/采样点范围。", channel));
                    break;
                default:
                    SetStatus(string.Format(CultureInfo.InvariantCulture, "CH{0} 配置回执异常，状态码=0x{1:X2}。", channel, status));
                    break;
            }
        }

        private void HandleDeviceInfoAck(byte status, byte[] payload)
        {
            if (status != ProtocolStatusOk || payload == null || payload.Length < 16 || payload[0] != ProtocolVersion)
            {
                SetStatus("设备信息回执无效。");
                return;
            }

            var major = payload[1];
            var minor = payload[2];
            var patch = payload[3];
            var featureFlags = ReadUInt32LittleEndian(payload, 4);
            var vid = ReadUInt16LittleEndian(payload, 8);
            var pid = ReadUInt16LittleEndian(payload, 10);
            var channels = payload[12];
            SetStatus(string.Format(CultureInfo.InvariantCulture,
                "设备在线: FW {0}.{1}.{2}, VID=0x{3:X4}, PID=0x{4:X4}, CH={5}, Features=0x{6:X8}",
                major, minor, patch, vid, pid, channels, featureFlags));
        }

        private void HandleChannelCapabilitiesAck(byte channel, byte status, byte[] payload)
        {
            if (status != ProtocolStatusOk || payload == null || payload.Length < 20 || payload[0] != ProtocolVersion)
            {
                return;
            }

            var flags = payload[2];
            var nominalMin = ReadUInt32LittleEndian(payload, 4);
            var nominalMax = ReadUInt32LittleEndian(payload, 8);
            var dataMax = ReadUInt32LittleEndian(payload, 12);
            SetStatus(string.Format(CultureInfo.InvariantCulture,
                "CH{0} 能力: {1}{2}{3}, N={4}-{5}, Dmax={6}",
                channel,
                (flags & 0x01) != 0 ? "CAN " : "",
                (flags & 0x02) != 0 ? "FD " : "",
                (flags & 0x04) != 0 ? "Term " : "",
                nominalMin,
                nominalMax,
                dataMax));
        }

        private void HandleRuntimeStatusAck(byte channel, byte status, byte[] payload)
        {
            if (status != ProtocolStatusOk || payload == null || payload.Length < 20 || payload[0] != ProtocolVersion)
            {
                return;
            }

            var flags = payload[1];
            var txCount = ReadUInt32LittleEndian(payload, 4);
            var rxCount = ReadUInt32LittleEndian(payload, 8);
            var lastError = ReadUInt32LittleEndian(payload, 12);
            var hostPending = payload[16];
            var uplinkPending = payload[17];
            var hostDropped = payload[18];
            var uplinkDropped = payload[19];
            SetStatus(string.Format(CultureInfo.InvariantCulture,
                "CH{0} 状态: flags=0x{1:X2}, tx={2}, rx={3}, err=0x{4:X2}, hostQ={5}, usbQ={6}, hostDrop={7}, usbDrop={8}",
                channel,
                flags,
                txCount,
                rxCount,
                lastError,
                hostPending,
                uplinkPending,
                hostDropped,
                uplinkDropped));
        }

        private void HandleHeartbeatAck(byte status, byte[] payload)
        {
            if (status != ProtocolStatusOk)
            {
                return;
            }

            if (payload != null &&
                payload.Length == LinkHeartbeatPayload.Length &&
                (payload.SequenceEqual(LinkHeartbeatPayload) || payload.SequenceEqual(UnlinkHeartbeatPayload)))
            {
                return;
            }

            var text = payload == null ? "" : BitConverter.ToString(payload);
            SetStatus(string.Format(CultureInfo.InvariantCulture, "设备心跳响应: {0}", text));
        }

        private static CanFrame ParsePacket(byte[] packet)
        {
            if (packet == null || packet.Length < 8 || packet[0] != PacketSync || (packet[3] & PacketFlagControl) != 0)
            {
                return null;
            }

            var dlc = packet[2];
            if (dlc > 64)
            {
                return null;
            }
            if (packet.Length < 8 + dlc)
            {
                return null;
            }

            var data = new byte[dlc];
            if (dlc > 0)
            {
                Buffer.BlockCopy(packet, 8, data, 0, dlc);
            }

            var flags = packet[3];
            var errorCode = (byte)((flags >> 4) & 0x0F);
            var id = (uint)(packet[4] | (packet[5] << 8) | (packet[6] << 16) | (packet[7] << 24));
            return new CanFrame
            {
                Timestamp = DateTime.Now,
                Channel = packet[1],
                Id = id,
                IsCanFd = (flags & PacketFlagCanFd) != 0,
                IsTx = (flags & PacketFlagTx) != 0,
                HasError = (flags & PacketFlagError) != 0,
                ErrorCode = errorCode,
                ErrorType = DecodeErrorType(errorCode),
                Data = data
            };
        }

        private static string DecodeErrorType(byte code)
        {
            switch (code)
            {
                case 0x1: return "Bit Error";
                case 0x2: return "Stuff Error";
                case 0x3: return "CRC Error";
                case 0x4: return "Form Error";
                case 0x5: return "ACK Error";
                case 0x6: return "Bus Off";
                case 0x7: return "Error Passive";
                case 0x8: return "Arbitration Lost";
                default: return code == 0 ? "None" : string.Format("Unknown(0x{0:X1})", code);
            }
        }

        private void OpenDocumentationEntry(string relativePath)
        {
            if (string.IsNullOrWhiteSpace(relativePath))
            {
                return;
            }

            var normalized = relativePath.Replace('/', Path.DirectorySeparatorChar).TrimStart(Path.DirectorySeparatorChar);
            var candidates = new List<string>();

            try
            {
                var repoRoot = TryFindRepoRoot();
                if (!string.IsNullOrWhiteSpace(repoRoot))
                {
                    candidates.Add(Path.GetFullPath(Path.Combine(repoRoot, normalized)));
                }
            }
            catch
            {
            }

            try
            {
                var baseDir = AppDomain.CurrentDomain.BaseDirectory ?? "";
                if (!string.IsNullOrWhiteSpace(baseDir))
                {
                    candidates.Add(Path.GetFullPath(Path.Combine(baseDir, normalized)));
                    candidates.Add(Path.GetFullPath(Path.Combine(baseDir, "..", normalized)));
                    candidates.Add(Path.GetFullPath(Path.Combine(baseDir, "..", "..", normalized)));
                    candidates.Add(Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", normalized)));
                }
            }
            catch
            {
            }

            try
            {
                candidates.Add(Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, normalized)));
            }
            catch
            {
            }

            var existing = candidates
                .Where(p => !string.IsNullOrWhiteSpace(p))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault(p => File.Exists(p) || Directory.Exists(p));

            if (string.IsNullOrWhiteSpace(existing))
            {
                MessageBox.Show(
                    "未找到文档路径: " + relativePath + Environment.NewLine + "请确认工程目录结构完整。",
                    "EverVance",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning);
                return;
            }

            try
            {
                Process.Start(new ProcessStartInfo(existing) { UseShellExecute = true });
                SetStatus("已打开文档: " + existing);
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    "打开文档失败: " + ex.Message + Environment.NewLine + existing,
                    "EverVance",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
            }
        }

        private static string TryFindRepoRoot()
        {
            try
            {
                var current = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory ?? Environment.CurrentDirectory);
                for (int i = 0; i < 12 && current != null; i++)
                {
                    if (Directory.Exists(Path.Combine(current.FullName, "Host_PC")))
                    {
                        return current.FullName;
                    }
                    current = current.Parent;
                }
            }
            catch
            {
            }
            return "";
        }

        private void ShowAboutDialog()
        {
            var root = TryFindRepoRoot();
            var info = new StringBuilder();
            info.AppendLine("EverVance 上位机");
            info.AppendLine("版本: 1.0");
            info.AppendLine("目标: USB(WinUSB Vendor) + CAN/CAN FD 调试与监控");
            info.AppendLine();
            info.AppendLine("文档入口:");
            info.AppendLine("1) Host_PC/README.md");
            info.AppendLine("2) Host_PC/EverVance/README.md");
            info.AppendLine("3) Host_PC/docs/");
            if (!string.IsNullOrWhiteSpace(root))
            {
                info.AppendLine();
                info.AppendLine("工程根目录:");
                info.AppendLine(root);
            }

            MessageBox.Show(
                info.ToString(),
                "关于 EverVance",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
        }

        private void SetStatus(string text)
        {
            if (_status != null)
            {
                _status.Text = text;
            }
        }

        private bool ShouldCommitChannelGridCurrentCellImmediately()
        {
            if (_channelGrid == null || _channelGrid.CurrentCell == null)
            {
                return false;
            }

            var col = _channelGrid.Columns[_channelGrid.CurrentCell.ColumnIndex];
            return col != null && IsImmediateCommitChannelColumn(col.DataPropertyName);
        }

        private static bool IsImmediateCommitChannelColumn(string dataPropertyName)
        {
            return dataPropertyName == "Enabled" ||
                   dataPropertyName == "FrameType" ||
                   dataPropertyName == "NominalSamplePreset" ||
                   dataPropertyName == "DataSamplePreset" ||
                   dataPropertyName == "TerminationEnabled";
        }

        private static bool IsDeferredNormalizeChannelColumn(string dataPropertyName)
        {
            return dataPropertyName == "NominalBitrate" ||
                   dataPropertyName == "NominalSamplePointText" ||
                   dataPropertyName == "DataBitrate" ||
                   dataPropertyName == "DataSamplePointText";
        }
    }
}
