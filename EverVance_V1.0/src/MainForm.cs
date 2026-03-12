using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using System.Windows.Forms.DataVisualization.Charting;

namespace EverVance
{
    public sealed class MainForm : Form
    {
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
        private bool _txScheduleRunning;
        private bool _projectReady;
        private bool _busViewDirty;
        private bool _projectDirty;
        private bool _suspendDirtyTracking;
        private bool _isConnected;
        private bool _rxErrorLatched;
        private bool _txErrorLatched;
        private DateTime _rxFlashUntilUtc = DateTime.MinValue;
        private DateTime _txFlashUntilUtc = DateTime.MinValue;

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
        private ComboBox _channelAddBox;
        private DataGridView _channelGrid;
        private ComboBox _txFrameChannelAddBox;
        private DataGridView _txFrameGrid;
        private TreeView _dbcTree;
        private Chart _chart;
        private DataGridView _grid;
        private TabControl _tabs;
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

        public MainForm()
        {
            Text = "EverVance";
            Width = 1680;
            Height = 980;
            MinimumSize = new Size(1360, 860);
            StartPosition = FormStartPosition.CenterScreen;
            KeyPreview = true;
            _appStatePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "EverVance", "appstate.ini");

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
            // 数据域波特率（仅 CAN FD 有效）
            public int DataBitrate { get; set; }
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
            nav.Dock = DockStyle.Top;

            _tabs = new TabControl();
            _tabs.Dock = DockStyle.Fill;
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
            _mainSplit.Panel2.Controls.Add(_tabs);

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
            PromptWorkspaceOnStartup();
            RefreshWorkspaceProjects();
            RefreshEndpointPresets(false);
            SetStatus("请选择 WorkSpace 下的工程并点击“开始工程”，或先新建工程。");
            WireProjectDirtyTracking();
            UpdateWindowTitle();
            UpdateActionStates();
            RenderIndicators();
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
            addChannelBtn.Text = "添加";
            addChannelBtn.Left = 164;
            addChannelBtn.Top = 7;
            addChannelBtn.Width = 80;
            addChannelBtn.Height = 28;
            addChannelBtn.Click += delegate { AddChannelConfig(); };
            channelTop.Controls.Add(addChannelBtn);

            var removeChannelBtn = new Button();
            removeChannelBtn.Text = "删除选中";
            removeChannelBtn.Left = 252;
            removeChannelBtn.Top = 7;
            removeChannelBtn.Width = 100;
            removeChannelBtn.Height = 28;
            removeChannelBtn.Click += delegate { RemoveSelectedChannelConfig(); };
            channelTop.Controls.Add(removeChannelBtn);

            var channelHint = new Label();
            channelHint.Text = "每通道固定帧格式。CAN 时 DataBitrate 列自动置灰且留空，CAN FD 才可配置。";
            channelHint.Left = 362;
            channelHint.Top = 12;
            channelHint.Width = 820;
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
            colType.Items.AddRange(new object[] { "CAN", "CAN FD" });
            colType.Width = 90;
            _channelGrid.Columns.Add(colType);

            var colNom = new DataGridViewTextBoxColumn();
            colNom.DataPropertyName = "NominalBitrate";
            colNom.HeaderText = "Nominal";
            colNom.Width = 110;
            _channelGrid.Columns.Add(colNom);

            var colData = new DataGridViewTextBoxColumn();
            colData.DataPropertyName = "DataBitrate";
            colData.HeaderText = "Data";
            colData.Width = 110;
            _channelGrid.Columns.Add(colData);

            _channelGrid.DataSource = _channelConfigs;
            _channelGrid.CellFormatting += ChannelGridCellFormatting;
            _channelGrid.CellBeginEdit += ChannelGridCellBeginEdit;
            _channelGrid.CellValueChanged += ChannelGridCellValueChanged;
            _channelGrid.CurrentCellDirtyStateChanged += delegate
            {
                if (_channelGrid.IsCurrentCellDirty)
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
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 30));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 25));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 45));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, 10));

            var topPanel = new Panel { Dock = DockStyle.Fill };
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
            right.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            right.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var rightTop = new Panel { Dock = DockStyle.Fill };
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
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            var top = new Panel { Dock = DockStyle.Fill };
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
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
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
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 86));
            panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));

            panel.Controls.Add(new Label { Text = "WorkSpace 工程列表", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);

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

            _workspaceProjectList = new ListBox();
            _workspaceProjectList.Dock = DockStyle.Fill;
            _workspaceProjectList.DoubleClick += delegate { StartSelectedWorkspaceProject(); };
            _workspaceProjectList.SelectedIndexChanged += delegate { UpdateActionStates(); };
            panel.Controls.Add(_workspaceProjectList, 0, 2);

            panel.Controls.Add(new Label { Text = "提示：双击左侧工程或点地址栏“开始工程”。", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 3);

            return panel;
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

            if (_tabs != null) _tabs.Enabled = _projectReady;
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

        private void PromptWorkspaceOnStartup()
        {
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = "请选择 WorkSpace 目录（将记住上次路径）";
                dlg.SelectedPath = string.IsNullOrWhiteSpace(_workspacePath) ? Environment.CurrentDirectory : _workspacePath;
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    SetWorkspacePath(dlg.SelectedPath);
                    SaveLastWorkspacePath(dlg.SelectedPath);
                }
            }
        }

        private string LoadLastWorkspacePath()
        {
            try
            {
                string path;
                if (_appState != null && _appState.TryGetValue("WorkspacePath", out path))
                {
                    if (!string.IsNullOrWhiteSpace(path) && Directory.Exists(path))
                    {
                        return path;
                    }
                }

                if (File.Exists(_appStatePath))
                {
                    var line = File.ReadAllText(_appStatePath, Encoding.UTF8).Trim();
                    if (!string.IsNullOrWhiteSpace(line) && Directory.Exists(line))
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
                _appState["WorkspacePath"] = path ?? "";
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
                _status.Text = string.Format("已连接: {0}", _transport.Name);
            }
            else
            {
                _isConnected = false;
                _status.Text = "连接失败";
            }
            UpdateActionStates();
            RenderIndicators();
        }

        private void DisconnectTransport()
        {
            if (_transport != null)
            {
                _transport.Close();
            }
            _isConnected = false;
            _status.Text = "已断开";
            UpdateActionStates();
            RenderIndicators();
        }

        private void AddChannelConfig()
        {
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
                FrameType = "CAN",
                NominalBitrate = 500000,
                DataBitrate = 2000000
            });
            SortChannelConfigs();
            SetStatus(string.Format("已添加通道 CH{0}", channelId));
        }

        private void RemoveSelectedChannelConfig()
        {
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
                MessageBox.Show("请先在发送帧列表中选择一行。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Information);
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
                MessageBox.Show("没有启用的发送帧。", "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Information);
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
                    MessageBox.Show(
                        string.Format("发送帧目标通道 CH{0} 未配置。请先在通道管理中添加。", row.ChannelId),
                        "EverVance",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Warning);
                    return false;
                }
                if (!cfg.Enabled)
                {
                    MessageBox.Show(
                        string.Format("发送帧目标通道 CH{0} 处于关闭状态。", row.ChannelId),
                        "EverVance",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Warning);
                    return false;
                }

                var frameType = (cfg.FrameType ?? "CAN").Trim().ToUpperInvariant();
                var isCanFd = frameType == "CAN FD";
                var id = ParseHexId(row.IdHex);
                var data = ParseHexBytes(row.DataHex ?? "");

                // 防呆：经典 CAN 最多 8 字节，CAN FD 最多 64 字节
                if (!isCanFd && data.Length > 8)
                {
                    MessageBox.Show(
                        string.Format("CH{0} 为 CAN，帧数据长度不能超过 8 字节。", cfg.ChannelId),
                        "EverVance",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Warning);
                    return false;
                }
                if (isCanFd && data.Length > 64)
                {
                    MessageBox.Show(
                        string.Format("CH{0} 为 CAN FD，帧数据长度不能超过 64 字节。", cfg.ChannelId),
                        "EverVance",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Warning);
                    return false;
                }

                byte flags = (byte)((isCanFd ? 0x01 : 0x00) | 0x02);
                var packet = BuildPacket((byte)cfg.ChannelId, id, flags, data);

                if (_transport.Send(packet))
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
                MessageBox.Show("发送参数错误: " + ex.Message, "EverVance", MessageBoxButtons.OK, MessageBoxIcon.Warning);
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

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null || col.DataPropertyName != "DataBitrate")
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            // CAN 模式禁止编辑 DataBitrate，防止误配置
            if (!string.Equals(row.FrameType, "CAN FD", StringComparison.OrdinalIgnoreCase))
            {
                e.Cancel = true;
            }
        }

        private void ChannelGridCellFormatting(object sender, DataGridViewCellFormattingEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0 || e.ColumnIndex < 0)
            {
                return;
            }

            var col = _channelGrid.Columns[e.ColumnIndex];
            if (col == null || col.DataPropertyName != "DataBitrate")
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            if (string.Equals(row.FrameType, "CAN FD", StringComparison.OrdinalIgnoreCase))
            {
                e.CellStyle.BackColor = Color.White;
                e.CellStyle.ForeColor = Color.Black;
                e.Value = row.DataBitrate.ToString(CultureInfo.InvariantCulture);
            }
            else
            {
                // CAN 模式下该栏位灰态且留空，避免文字误导
                e.CellStyle.BackColor = Color.Gainsboro;
                e.CellStyle.ForeColor = Color.DimGray;
                e.Value = "";
            }
            e.FormattingApplied = true;
        }

        private void ChannelGridCellValueChanged(object sender, DataGridViewCellEventArgs e)
        {
            if (_channelGrid == null || e.RowIndex < 0)
            {
                return;
            }

            var row = _channelGrid.Rows[e.RowIndex].DataBoundItem as ChannelConfigRow;
            if (row == null)
            {
                return;
            }

            if (!string.Equals(row.FrameType, "CAN FD", StringComparison.OrdinalIgnoreCase))
            {
                row.FrameType = "CAN";
                row.DataBitrate = 0;
            }
            else if (row.DataBitrate <= 0)
            {
                row.DataBitrate = 2000000;
            }

            SortChannelConfigs();
            _channelGrid.InvalidateRow(e.RowIndex);
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
                lines.Add(string.Format("{0}\t{1}\t{2}\t{3}\t{4}",
                    c.ChannelId, c.Enabled ? 1 : 0, EncodeProjectValue(c.FrameType ?? "CAN"), c.NominalBitrate, c.DataBitrate));
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
                    _channelConfigs.Add(new ChannelConfigRow
                    {
                        ChannelId = ParseIntSafe(p[0], 0),
                        Enabled = ParseIntSafe(p[1], 1) != 0,
                        FrameType = DecodeProjectValue(p[2]),
                        NominalBitrate = ParseIntSafe(p[3], 500000),
                        DataBitrate = ParseIntSafe(p[4], 0)
                    });
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
            buf[0] = 0xA5;
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

        private static CanFrame ParsePacket(byte[] packet)
        {
            if (packet == null || packet.Length < 8 || packet[0] != 0xA5)
            {
                return null;
            }

            var dlc = packet[2];
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
                IsCanFd = (flags & 0x01) != 0,
                IsTx = (flags & 0x02) != 0,
                HasError = (flags & 0x04) != 0,
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

        private void SetStatus(string text)
        {
            if (_status != null)
            {
                _status.Text = text;
            }
        }
    }
}
