/*
 * main.cpp —— i.MX6ULL 外设控制台（Qt Widgets，全屏 1024×600）
 *
 * 界面只操作设备节点，不碰寄存器：
 *   /dev/led、/dev/beep 写入命令字符；/dev/key 通过 QSocketNotifier 接收按键事件
 * 另外显示背光、温度、可用内存、运行时间，并提供关机与重启入口。
 */

#include <QtWidgets>
#include <QDateTime>
#include <QMouseEvent>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>

/* 三个设备节点由 chrdevbase.ko 创建，节点名固定，主设备号动态分配 */
static const char *LED_DEV  = "/dev/led";
static const char *BEEP_DEV = "/dev/beep";
static const char *KEY_DEV  = "/dev/key";

/* 读 sysfs / procfs 里的文本，读不到返回空串 */
static QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

/* 读整数，解析失败时返回调用方给的默认值 */
static int readIntDefault(const QString &path, int def)
{
    bool ok = false;
    int v = readText(path).toInt(&ok);
    return ok ? v : def;
}

/*
 * 点哪跳哪的滑条。
 * QSlider 默认只有拖滑块才改值，点滑槽只走 pageStep；电容屏上小滑块又难按准，
 * 所以重写按下事件：按点击位置占轨道的比例算出档位，先 setValue 过去，
 * 再交给父类处理，拖动能力保持不变。
 */
class JumpSlider : public QSlider {
public:
    explicit JumpSlider(Qt::Orientation o, QWidget *p = nullptr) : QSlider(o, p) {}
protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        int span = maximum() - minimum();
        int v = minimum();
        if (width() > 1) {                          /* 防止宽度为 0 时除零 */
            double r = double(e->x()) / double(width());
            if (r < 0.0) r = 0.0;
            if (r > 1.0) r = 1.0;
            v = minimum() + int(span * r + 0.5);    /* 比例换算成档位，四舍五入 */
        }
        setValue(v);
        QSlider::mousePressEvent(e);
    }
};

/*
 * 主窗口。
 * 构造时先搭界面、再打开设备节点，然后起一个 1 秒定时器刷新底部状态栏。
 */
class Console : public QWidget {
public:
    explicit Console(QWidget *parent = nullptr)
        : QWidget(parent), ledFd(-1), beepFd(-1), keyFd(-1), ledOn(false), beepOn(false)
    {
        setWindowTitle("IMX6ULL Peripheral Console");
        buildUi();
        openDevice();

        QTimer *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this]() { updateStatus(); });
        t->start(1000);
        updateStatus();
    }

private:
    int ledFd, beepFd, keyFd;           /* 三个节点的文件描述符，-1 表示未打开 */
    bool ledOn, beepOn;                 /* 界面记录的开关状态，决定下次发什么命令 */

    QPushButton *ledBtn, *beepBtn;
    QPushButton *powerBtn, *rebootBtn;
    QSlider     *bklSlider;
    QLabel      *bklLbl, *keyLbl, *cntLbl, *envLbl, *msgLbl;

    /*
     * 搭界面：标题 → LED / 蜂鸣器按钮 → 背光行 → 按键与次数 → 系统信息 → 电源行。
     * 所有控件、样式、布局和信号连接都在这个函数里完成。
     */
    void buildUi()
    {
        QLabel *title = new QLabel("IMX6ULL Peripheral Console", this);
        QFont tf = title->font(); tf.setPointSize(30); tf.setBold(true);
        title->setFont(tf);
        title->setAlignment(Qt::AlignCenter);

        /* 两个开关按钮做高做大，方便手指点按 */
        ledBtn  = new QPushButton("LED : OFF", this);
        beepBtn = new QPushButton("BEEP : OFF", this);
        QFont bf = ledBtn->font(); bf.setPointSize(26);
        ledBtn->setFont(bf); beepBtn->setFont(bf);
        ledBtn->setMinimumHeight(90); beepBtn->setMinimumHeight(90);

        /* 背光：值一变就写 sysfs，加减按钮只改滑条数值，由滑条统一写硬件 */
        bklLbl = new QLabel("BACKLIGHT", this);
        bklSlider = new JumpSlider(Qt::Horizontal, this);
        bklSlider->setRange(1, 7);
        bklSlider->setPageStep(1);
        bklSlider->setMinimumHeight(72);
        bklSlider->setStyleSheet(
            "QSlider::groove:horizontal{height:16px;background:#cfcfcf;border-radius:8px;}"
            "QSlider::sub-page:horizontal{background:#4a90d9;border-radius:8px;}"
            "QSlider::handle:horizontal{width:56px;height:56px;margin:-20px 0;border-radius:28px;background:#2f6fb5;}");
        bklSlider->setValue(readIntDefault("/sys/class/backlight/backlight/brightness", 6));

        connect(bklSlider, &QSlider::valueChanged, this, [this](int v) {
            QFile f("/sys/class/backlight/backlight/brightness");
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QString::number(v).toLatin1());
                f.close();
            }
            updateBacklightLabel();
        });

        QPushButton *bklDn = new QPushButton("-", this);
        QPushButton *bklUp = new QPushButton("+", this);
        QFont sf = bklDn->font(); sf.setPointSize(24); sf.setBold(true);
        bklDn->setFont(sf); bklUp->setFont(sf);
        bklDn->setMinimumSize(72, 72);
        bklUp->setMinimumSize(72, 72);

        bklDn->setMaximumWidth(80);                 /* 限宽，把宽度留给滑条 */
        bklUp->setMaximumWidth(80);
        connect(bklDn, &QPushButton::clicked, this, [this]() {
            bklSlider->setValue(bklSlider->value() - 1);
        });
        connect(bklUp, &QPushButton::clicked, this, [this]() {
            bklSlider->setValue(bklSlider->value() + 1);
        });

        keyLbl = new QLabel("KEY0 : HIGH (released)", this);
        cntLbl = new QLabel("press : 0", this);
        envLbl = new QLabel("", this);
        msgLbl = new QLabel("ready", this);

        QFont lf = keyLbl->font(); lf.setPointSize(20);
        keyLbl->setFont(lf); cntLbl->setFont(lf);
        envLbl->setFont(lf); msgLbl->setFont(lf);
        bklLbl->setFont(lf);

        powerBtn = new QPushButton("POWER OFF", this);
        rebootBtn = new QPushButton("REBOOT", this);
        QFont pf = powerBtn->font(); pf.setPointSize(16);
        powerBtn->setFont(pf); rebootBtn->setFont(pf);
        powerBtn->setMinimumHeight(56); rebootBtn->setMinimumHeight(56);

        powerBtn->setMinimumWidth(220);
        rebootBtn->setMinimumWidth(160);
        powerBtn->setStyleSheet("QPushButton{background:#b54a3a;color:white;border-radius:8px;}"
                                "QPushButton:pressed{background:#8f3a2d;}");
        rebootBtn->setStyleSheet("QPushButton{background:#3a6fb5;color:white;border-radius:8px;}"
                                 "QPushButton:pressed{background:#2d5890;}");

        /* 网格布局：第 0 行标题，1 两个开关，2 背光，3–4 状态，5 提示与电源 */
        QGridLayout *g = new QGridLayout(this);
        g->setContentsMargins(16, 10, 16, 10);
        g->setHorizontalSpacing(16);
        g->setVerticalSpacing(10);
        g->addWidget(title,    0, 0, 1, 4);
        g->addWidget(ledBtn,   1, 0, 1, 2);
        g->addWidget(beepBtn,  1, 2, 1, 2);
        g->addWidget(bklLbl,   2, 0, 1, 1);
        g->addWidget(bklDn,    2, 1, 1, 1);
        g->addWidget(bklSlider, 2, 2, 1, 1);
        g->addWidget(bklUp,    2, 3, 1, 1);
        g->addWidget(keyLbl,   3, 0, 1, 2);
        g->addWidget(cntLbl,   3, 2, 1, 2);
        g->addWidget(envLbl,   4, 0, 1, 4);

        /* 最后一行单独包一个水平布局：提示文字占满剩余宽度，右侧放两个电源按钮 */
        QWidget *row5 = new QWidget(this);
        QHBoxLayout *h5 = new QHBoxLayout(row5);
        h5->setContentsMargins(0, 0, 0, 0);
        h5->setSpacing(16);
        h5->addWidget(msgLbl, 1);
        h5->addWidget(powerBtn);
        h5->addWidget(rebootBtn);
        g->addWidget(row5, 5, 0, 1, 4);
        g->setRowStretch(1, 1);                     /* 按钮行吃掉多余高度 */

        /* 关机、重启都要二次确认，默认停在 No 防误触 */
        connect(powerBtn, &QPushButton::clicked, this, [this]() {
            if (QMessageBox::question(this, "Confirm",
                    "Power off now?\nThe two drivers will be unloaded first.",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                doPower(false);
        });
        connect(rebootBtn, &QPushButton::clicked, this, [this]() {
            if (QMessageBox::question(this, "Confirm",
                    "Reboot now?\nThe two drivers will be unloaded first.",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                doPower(true);
        });

        /* 开关按钮：按当前状态发相反的字符，再翻转状态与按钮文字 */
        connect(ledBtn, &QPushButton::clicked, this, [this]() {
            sendCmd(ledOn ? '0' : '1');
            ledOn = !ledOn;
            ledBtn->setText(ledOn ? "LED : ON" : "LED : OFF");
        });
        connect(beepBtn, &QPushButton::clicked, this, [this]() {
            sendCmd(beepOn ? 'B' : 'b');
            beepOn = !beepOn;
            beepBtn->setText(beepOn ? "BEEP : ON" : "BEEP : OFF");
        });

        updateBacklightLabel();
    }

    void updateBacklightLabel()
    {
        bklLbl->setText(QString("BACKLIGHT %1/7").arg(bklSlider->value()));
    }

    /*
     * 关机 / 重启。
     * 步骤：关掉三个设备节点 → 后台子 shell 里等 1 秒 → 结束界面进程
     *       → rmmod 两个驱动 → sync → poweroff 或 reboot。
     * 必须放到后台并延时，因为要结束的正是当前进程，驱动也要等节点释放后才能卸载。
     * 日志写在 /tmp/power.log。
     */
    void doPower(bool reboot)
    {
        if (ledFd >= 0)  { ::close(ledFd);  ledFd = -1; }
        if (beepFd >= 0) { ::close(beepFd); beepFd = -1; }
        if (keyFd >= 0)  { ::close(keyFd);  keyFd = -1; }
        msgLbl->setText(reboot ? "rebooting ..." : "powering off ...");

        QString cmd = "(sleep 1; pkill console; sleep 1; "
                      "rmmod gt9xx; rmmod chrdevbase; sync; ";
        cmd += reboot ? "reboot" : "poweroff -f";
        cmd += ") >/tmp/power.log 2>&1 &";
        system(cmd.toLatin1().constData());
    }

    /*
     * 打开三个设备节点，全部用非阻塞方式，避免读按键时卡住界面线程。
     * 按键另外挂一个 QSocketNotifier 监听可读事件：底层就是驱动的 poll，
     * 有事件时回调 onKeyEvent，等价于把 poll 接进 Qt 事件循环，不用另开线程。
     */
    void openDevice()
    {
        ledFd  = ::open(LED_DEV,  O_RDWR | O_NONBLOCK);
        beepFd = ::open(BEEP_DEV, O_RDWR | O_NONBLOCK);
        keyFd  = ::open(KEY_DEV,  O_RDWR | O_NONBLOCK);

        if (ledFd < 0 || beepFd < 0 || keyFd < 0)
            msgLbl->setText("open /dev/led, /dev/beep or /dev/key failed");

        if (keyFd >= 0) {
            QSocketNotifier *n = new QSocketNotifier(keyFd, QSocketNotifier::Read, this);
            connect(n, &QSocketNotifier::activated, this, [this]() { onKeyEvent(); });
        }
    }

    /*
     * 往驱动写一个字符。'b' / 'B' 发给蜂鸣器，'1' / '0' 发给 LED，
     * 上层只传字符，由这里决定写到哪个节点。
     */
    void sendCmd(char c)
    {
        bool isBeep = (c == 'b' || c == 'B');
        int  target = isBeep ? beepFd : ledFd;

        if (target < 0) {
            msgLbl->setText("device not open");
            return;
        }
        if (::write(target, &c, 1) != 1) {          /* 成功写入返回 1 */
            msgLbl->setText("write failed");
            return;
        }
        msgLbl->setText(QString("write '%1' -> %2").arg(c).arg(isBeep ? "beep" : "led"));
    }

    /*
     * 按键事件回调：读到 0 表示按下，更新标签并顺带翻转一次 LED；
     * 读到 1 表示松开，只更新标签。
     */
    void onKeyEvent()
    {
        char c = 1;
        if (keyFd < 0)
            return;
        if (::read(keyFd, &c, 1) <= 0)
            return;

        if (c == 0) {
            keyLbl->setText("KEY0 : LOW (pressed)");
            sendCmd(ledOn ? '0' : '1');
            ledOn = !ledOn;
            ledBtn->setText(ledOn ? "LED : ON" : "LED : OFF");
            msgLbl->setText("KEY0 interrupt -> LED toggled");
        } else {
            keyLbl->setText("KEY0 : HIGH (released)");
        }
        updateStatus();
    }

    /*
     * 每秒刷新底部状态栏，数据全部来自内核的轻量接口：
     *   /proc/chrdevbase_key                  按键次数与当前电平
     *   /sys/class/thermal/thermal_zone0/temp 芯片温度，单位毫摄氏度
     *   /proc/meminfo                         MemAvailable，单位 kB
     *   /proc/uptime                          开机秒数
     */
    void updateStatus()
    {
        QString kp = readText("/proc/chrdevbase_key");
        if (kp.contains("level=0"))
            keyLbl->setText("KEY0 : LOW (pressed)");
        else if (kp.contains("level=1"))
            keyLbl->setText("KEY0 : HIGH (released)");

        int press = 0;
        int idx = kp.indexOf("press=");
        if (idx >= 0)                               /* 从 "press=12 level=..." 里抠数字 */
            press = kp.mid(idx + 6, 10).section(' ', 0, 0).toInt();
        cntLbl->setText(QString("press : %1").arg(press));

        bool ok = false;
        int milli = readText("/sys/class/thermal/thermal_zone0/temp").toInt(&ok);
        QString temp = ok ? QString("%1C").arg(milli / 1000.0, 0, 'f', 1) : QString("NA");

        QString meminfo = readText("/proc/meminfo");
        int memAvail = 0;
        for (const QString &line : meminfo.split('\n')) {
            if (line.startsWith("MemAvailable:")) {
                memAvail = line.section(' ', -2, -2).toInt();   /* 倒数第二个字段是数值 */
                break;
            }
        }

        double up = readText("/proc/uptime").section(' ', 0, 0).toDouble(&ok);
        int upSec = ok ? (int)up : 0;

        envLbl->setText(QString("TEMP %1   MEM %2MB   UP %3min%4s   %5")
                            .arg(temp)
                            .arg(memAvail / 1024)               /* kB → MB */
                            .arg(upSec / 60)
                            .arg(upSec % 60)
                            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    }
};

/* 程序入口：无边框全屏，铺满 1024×600 的屏 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Console w;
    w.setWindowFlags(Qt::FramelessWindowHint);
    w.resize(1024, 600);
    w.showFullScreen();
    return app.exec();
}
