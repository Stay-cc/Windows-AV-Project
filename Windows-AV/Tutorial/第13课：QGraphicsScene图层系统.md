# 第13课：QGraphicsScene 图层系统

> **目标**：深入理解项目如何用 `QGraphicsScene` / `QGraphicsView` 搭建一个可视化图层编辑区，掌握 `ResizablePixmapItem`、`ResizableTextItem` 的拖拽缩放逻辑、Z 轴排序、吸附辅助线、前后端图层同步，以及为什么这种方案特别适合录屏叠加层编辑。  
> **对应文件**：`ResizablePixmapItem.h`、`ResizablePixmapItem.cpp`、`ResizableTextItem.h`、`mainwindow.cpp`

---

## 1. 第13课和第12课的关系

第12课我们刚讲完：
- 无边框窗口外壳
- 自定义标题栏
- 主窗口整体布局

但主窗口里最有“编辑器味道”的部分，其实是中间那块画布区域。

这块区域不是普通表单控件，而是一个能够承载：
- 背景预览
- 摄像头叠加
- 图片叠加
- 文本叠加
- 图层上下顺序
- 拖动与缩放

的可视编辑系统。

而项目实现这一层的核心选择是：

> **Qt Graphics View Framework：`QGraphicsScene + QGraphicsView + 自定义 QGraphicsItem`**

所以可以把第12、13课连起来理解：

```text
第12课：窗口壳子和整体布局
    ↓
第13课：窗口中间那块“可编辑画布”如何工作
```

---

## 2. 为什么这里不用普通 QWidget，而用 `QGraphicsScene`

在 `mainwindow.cpp:146`-`159`：

```cpp
m_scene = new QGraphicsScene(this);
m_view = new QGraphicsView(m_scene);
...
m_bgItem = new QGraphicsPixmapItem(defaultBg);
m_scene->addItem(m_bgItem);
```

这说明预览区不是用一个简单的 `QLabel` 去显示图像，而是把整块区域当成“场景”。

### 2.1 `QGraphicsScene` 适合什么场景

它非常适合：
- 场景里有多个对象
- 每个对象都能单独移动、缩放、选中
- 对象之间有层级关系
- 希望做类似画布/编辑器的交互

而当前项目的叠加层编辑正好符合这些特征。

### 2.2 如果用普通 QWidget 会有什么问题

当然也能做，但会更麻烦：
- 每个叠加层都要自己管理坐标系统
- 选中框要自己画
- 移动、缩放、层级、重绘区域都要自己协调
- 背景预览和前景叠加要自己处理绘制顺序

而 `QGraphicsScene` 已经天然提供了：
- item 管理
- 场景坐标系
- Z 轴
- 选中机制
- 鼠标事件分发

所以它是更合适的工具。

---

## 3. 当前场景里有哪些类型的“图层”

从 `mainwindow.cpp` 可以看出，当前项目至少支持三类叠加层：

1. 图片层
2. 文字层
3. 摄像头层

这些都会进入：

```cpp
QMap<int, ResizablePixmapItem*> m_overlayItems;
```

位置在 `mainwindow.h:109`。

### 3.1 为什么都能放进同一个容器

因为：
- 图片层本来就是 `ResizablePixmapItem`
- 摄像头层也用 `ResizablePixmapItem` 占位/展示
- 文字层 `ResizableTextItem` 继承自 `ResizablePixmapItem`

所以从主窗口视角看，它们都满足同一种“可移动、可缩放、可选中图层”的接口。

这说明作者的抽象不是按“内容类型”分，而是按“交互行为”分：

> **只要是画布上的一个可操作图层，就尽量统一成同一种 item 基类。**

---

## 4. 为什么 `ResizablePixmapItem` 继承 `QGraphicsObject`

在 `ResizablePixmapItem.h:9`：

```cpp
class ResizablePixmapItem : public QGraphicsObject
```

### 4.1 为什么不是 `QGraphicsPixmapItem`

因为作者不仅需要：
- 显示 pixmap

还需要：
- 自定义绘制选中框
- 自定义缩放手柄
- 响应 hover / press / move / release
- 发出 `geometryChanged` 信号

而 `QGraphicsObject` 兼具：
- `QGraphicsItem` 的场景能力
- `QObject` 的信号槽能力

这正适合当前需求。

### 4.2 这里的核心权衡

如果直接继承 `QGraphicsPixmapItem`：
- 显示图片更方便
- 但信号槽能力不天然具备

如果继承 `QGraphicsObject`：
- 要自己实现 `boundingRect()` 和 `paint()`
- 但换来更灵活的交互与通信能力

项目显然更看重后者。

---

## 5. `ResizablePixmapItem` 的核心职责是什么

从 `ResizablePixmapItem.h` 可以看出，它主要承担五类职责：

### 5.1 图像显示

- 持有 `QPixmap`
- 在 `paint()` 里绘制

### 5.2 几何控制

- 持有 `QRectF m_rect`
- 控制当前显示矩形大小

### 5.3 交互行为

- 可移动
- 可选中
- 可缩放

### 5.4 辅助反馈

- 缩放手柄
- 吸附辅助线
- 蜂鸣反馈

### 5.5 向外同步几何变化

通过信号：

```cpp
void geometryChanged(int id, QRect rect);
```

把用户在 UI 上的操作同步给主窗口和后端。

也就是说，这个类不是一个简单“图片显示控件”，而是一个：

> **支持编辑行为的场景图层对象。**

---

## 6. `m_rect` 为什么比 `m_pixmap.rect()` 更重要

在构造函数 `ResizablePixmapItem.cpp:16`：

```cpp
m_rect = m_pixmap.rect();
```

但后续并不是一直用原始像素尺寸，而是维护自己的 `m_rect`。

### 6.1 为什么不能只依赖原始 pixmap 尺寸

因为场景中的图层需要支持：
- 缩放显示
- 分辨率自适应
- 保持当前位置和当前显示大小

如果每次都直接画原始 pixmap：
- 图层尺寸会被原图分辨率绑死
- 无法自然表示“这个图层现在被用户拉到多大”

### 6.2 当前设计的含义

- `m_pixmap` 表示内容数据
- `m_rect` 表示当前显示几何

这是一种很重要的分离：

```text
内容 ≠ 显示尺寸
```

在编辑器/画布系统里，这几乎是基础思想。

---

## 7. `setPixmap()` 为什么不重置 `m_rect`

在 `ResizablePixmapItem.cpp:29`：

```cpp
void ResizablePixmapItem::setPixmap(const QPixmap &pixmap) {
    m_pixmap = pixmap;
    // 不重置 m_rect，保持当前的大小和位置，只更新内容
    update();
}
```

### 7.1 这行注释非常关键

它说明作者明确区分了：
- 图层内容变了
- 图层几何不一定要变

### 7.2 为什么这很合理

比如摄像头层：
- 用户已经把它拖到右下角，并调成一个合适大小
- 后续摄像头画面每帧都在更新

如果每次内容刷新都重置矩形：
- 用户布局会被冲掉
- 画面会不停跳尺寸

所以正确做法是：
- 内容更新只改像素
- 几何状态保持不变

这正是当前实现的做法。

---

## 8. 为什么构造函数里要启用这些 flag

在 `ResizablePixmapItem.cpp:17`-`18`：

```cpp
setAcceptHoverEvents(true);
setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
```

### 8.1 `setAcceptHoverEvents(true)`

作用：
- 让 item 能收到鼠标悬停事件
- 从而改变光标样式

如果没有它：
- `hoverMoveEvent()` 不会正常触发
- 用户就看不到缩放手柄区域的光标变化

### 8.2 `ItemIsMovable`

作用：
- 允许 item 被拖拽移动

这意味着作者把“普通拖动移动”交给 Qt 的 item 机制，而不是完全手写。

### 8.3 `ItemIsSelectable`

作用：
- 允许 item 被选中

于是后面可以根据 `isSelected()` 决定：
- 是否画边框
- 是否显示缩放手柄

### 8.4 `ItemSendsGeometryChanges`

作用：
- 当位置发生变化时，`itemChange()` 会收到通知

这正是吸附与几何同步逻辑的入口。

---

## 9. `boundingRect()` 为什么要比真实矩形稍微大一点

在 `ResizablePixmapItem.cpp:50`：

```cpp
QRectF ResizablePixmapItem::boundingRect() const { return m_rect.adjusted(-5, -5, 5, 5); }
```

### 9.1 为什么不是直接返回 `m_rect`

因为 item 除了主体内容，还会绘制：
- 选中虚线框
- 右下角缩放手柄

如果 `boundingRect()` 只等于内容矩形：
- 额外绘制区域可能超出边界
- Qt 的重绘裁剪可能把这些边缘效果截掉

### 9.2 这说明什么

`boundingRect()` 表示的是：
- 这个 item 可能影响到的完整绘制区域

而不只是“图片本身的内容区域”。

这是 Graphics View 里非常关键的概念。

---

## 10. `paint()`：图层是怎么被画出来的

在 `ResizablePixmapItem.cpp:54`：

```cpp
painter->drawPixmap(m_rect.toRect(), m_pixmap);
```

### 10.1 这行代码意味着什么

它会把 `m_pixmap` 拉伸绘制到 `m_rect` 对应区域。

所以你可以理解为：
- pixmap 是源内容
- `m_rect` 是当前显示框
- 绘制时自动做了缩放适配

### 10.2 选中态如何体现

如果 item 被选中：

```cpp
if (isSelected()) {
    QPen pen(Qt::blue, 2, Qt::DashLine);
    painter->drawRect(m_rect);
    painter->drawRect(m_rect.right() - m_handleSize, m_rect.bottom() - m_handleSize, m_handleSize, m_handleSize);
}
```

这会画出：
- 蓝色虚线框
- 右下角缩放手柄

### 10.3 这背后的交互思想

作者没有总是显示手柄，而是：
- 只有选中时才显示

这样好处是：
- 平时画面干净
- 编辑时有明确反馈

这非常符合图层编辑器的常见体验。

---

## 11. 为什么缩放区域只放在右下角

在 `ResizablePixmapItem.cpp:51`：

```cpp
return (pos.x() >= m_rect.right() - m_handleSize && pos.x() <= m_rect.right() && pos.y() >= m_rect.bottom() - m_handleSize && pos.y() <= m_rect.bottom());
```

### 11.1 当前实现的选择

它只把右下角一小块区域当作缩放手柄。

### 11.2 这样做的好处

实现简单，且不容易和拖动操作冲突：
- 大部分区域拖动移动
- 右下角专门缩放

### 11.3 它的局限

相比更成熟的编辑器，这种做法还不够完整。

例如暂时没有：
- 八方向缩放手柄
- 保持比例缩放快捷键
- 旋转控制点

但对当前项目来说，这已经足够实用，而且学习成本低。

---

## 12. `hoverMoveEvent()` 为什么只改光标不做别的

在 `ResizablePixmapItem.cpp:70`：

```cpp
if (isSelected() && isInResizeArea(event->pos())) setCursor(Qt::SizeFDiagCursor); else setCursor(Qt::ArrowCursor);
```

### 12.1 这在传达什么信息

用户只有在：
- 当前 item 已经被选中
- 鼠标又移动到右下角手柄区

才会看到对角缩放光标。

这等于告诉用户：

> 这个区域现在可以拉伸。

### 12.2 为什么未选中时不直接显示缩放光标

因为如果未选中也显示缩放态：
- 交互语义会变乱
- 用户不清楚当前是选中还是直接操作

所以这里采用了两步式交互：
1. 先选中
2. 再缩放

这更稳定，也更符合桌面编辑器习惯。

---

## 13. `mousePressEvent()` 如何区分“开始缩放”还是“正常选中/拖动”

在 `ResizablePixmapItem.cpp:75`：

```cpp
if (event->button() == Qt::LeftButton) {
    if (isSelected() && isInResizeArea(event->pos())) {
        m_isResizing = true; m_startPos = event->pos(); m_initialRect = m_rect; event->accept(); hideGuideLines();
    } else {
        QGraphicsObject::mousePressEvent(event);
    }
}
```

### 13.1 缩放模式的启动条件

只有同时满足：
- 左键点击
- item 已被选中
- 点击点位于右下角手柄区域

才进入缩放模式。

### 13.2 为什么其他情况直接交给基类

因为基类已经能处理：
- 选中
- 拖动移动
- 场景事件分发

作者只接管“需要特殊处理”的分支，把普通行为继续交给 Qt。

这说明当前实现并没有把整个交互系统从头重写，而是：

> **在 Qt 默认图元行为之上做增强。**

---

## 14. `mouseMoveEvent()` 中缩放逻辑是怎么工作的

在 `ResizablePixmapItem.cpp:85` 开始：

```cpp
QPointF diff = event->pos() - m_startPos;
qreal newW = qMax(50.0, m_initialRect.width() + diff.x());
qreal newH = qMax(50.0, m_initialRect.height() + diff.y());
```

### 14.1 核心思路

- 记录按下时的起点 `m_startPos`
- 记录当时矩形 `m_initialRect`
- 鼠标移动多少，就给宽高加多少

### 14.2 为什么用 `m_initialRect` 而不是当前 `m_rect`

因为这样可以避免连续叠加误差。

如果每一帧都在当前矩形基础上继续累积：
- 容易引入漂移
- 拖动手感不稳定

用“按下瞬间的初值 + 当前相对位移”更稳。

### 14.3 为什么设置最小值 `50.0`

```cpp
qMax(50.0, ...)
```

作用：
- 防止图层被拉得过小
- 保证手柄还能操作
- 避免出现几乎不可见的小对象

---

## 15. 为什么缩放时还要限制在场景边界内

在 `ResizablePixmapItem.cpp:90`-`94`：

```cpp
if (pos.x() + newW > sceneRect.right()) newW = sceneRect.right() - pos.x();
if (pos.y() + newH > sceneRect.bottom()) newH = sceneRect.bottom() - pos.y();
```

### 15.1 这在避免什么

避免用户把图层拉到场景外面去。

对录屏叠加系统来说，这种限制很合理，因为：
- 画布对应最终输出画面
- 叠加层跑出画面外通常没有意义
- 还会增加后端同步复杂度

### 15.2 这里体现的产品假设

当前项目默认认为：

> **可编辑图层应该始终落在最终画布范围内。**

这是比“无限画布”更偏向录屏工具的设计。

---

## 16. 为什么缩放后要立刻发 `geometryChanged`

在 `ResizablePixmapItem.cpp:96`：

```cpp
emit geometryChanged(m_id, QRect(this->x(), this->y(), m_rect.width(), m_rect.height()));
```

### 16.1 这意味着 UI 不是孤立运行的

用户在画布里拉大一个图层，不只是视觉上改变，后端也必须知道：
- 这个图层现在多大
- 应该合成到哪里

### 16.2 为什么信号里要带 `id`

因为主窗口维护的是多个叠加层。

只有带上 `id`，外部才能知道：
- 改的是哪一个图层

所以 `geometryChanged` 的作用是：

> **把场景编辑动作同步成后端可消费的几何更新事件。**

---

## 17. `itemChange()` 为什么是整个吸附系统的关键入口

在 `ResizablePixmapItem.cpp:107`：

```cpp
QVariant ResizablePixmapItem::itemChange(GraphicsItemChange change, const QVariant &value)
```

并且只在：

```cpp
if (change == ItemPositionChange && scene())
```

时处理。

### 17.1 为什么移动逻辑不直接写在 `mouseMoveEvent()` 里

因为普通拖动移动已经由 Qt 的 `ItemIsMovable` 机制负责。

作者真正关心的是：
- 当 Qt 计算出一个“即将移动到的新位置”时
- 我能不能拦截一下，做吸附和边界修正

而 `itemChange(ItemPositionChange, value)` 正好提供了这个切点。

### 17.2 这比手写拖动更优雅的地方

你不用自己完整重写“拖动 item”流程，
只需要：
- 让 Qt 负责基础移动
- 自己在关键变化点上修正结果

这是一种非常适合 Qt Graphics View 的实现方式。

---

## 18. 吸附逻辑到底在吸附什么

在 `ResizablePixmapItem.cpp:117`-`124`，作者检查了：

- 中心线对齐
- 左边缘对齐
- 右边缘对齐
- 顶边对齐
- 底边对齐

例如：

```cpp
if (std::abs((x + w/2) - sceneRect.center().x()) < SNAP_THRESHOLD)
```

表示：
- 如果 item 的中心点接近场景中心线
- 就自动吸附到中心位置

### 18.1 为什么只吸附到场景边和中心，而不是吸附别的图层

因为这是一个更简单、也更稳定的第一版实现。

它支持的对齐语义是：
- 相对于整个画布居中
- 相对于整个画布贴边

而不是图层与图层之间的复杂对齐。

### 18.2 为什么这已经很实用

对于录屏叠加来说，用户最常见需求就是：
- 摄像头放在左下/右下
- Logo 贴边
- 标题居中

所以相对画布的吸附已经能覆盖大量场景。

---

## 19. 绿色辅助线为什么单独用 `QGraphicsLineItem`

在 `ResizablePixmapItem.cpp:43`-`47`：

```cpp
if (!m_vLine) { m_vLine = new QGraphicsLineItem(); ... scene()->addItem(m_vLine); }
if (!m_hLine) { m_hLine = new QGraphicsLineItem(); ... scene()->addItem(m_hLine); }
```

### 19.1 为什么不直接在 item 自己的 `paint()` 里画辅助线

因为辅助线并不属于 item 自身内容，它代表的是：
- 整个场景里的对齐参考

如果画在 item 内部：
- 很难自然跨越整个场景
- 也不利于单独控制显示/隐藏

### 19.2 单独作为场景 item 的好处

- 可以跨场景整条显示
- 可以独立设置 Z 值
- 可以在需要时显示，不需要时隐藏
- 不污染原图层本身的绘制逻辑

这是很合理的拆分。

---

## 20. 为什么辅助线的 `ZValue` 设为 100

在 `ResizablePixmapItem.cpp:45`-`46`：

```cpp
m_vLine->setZValue(100);
m_hLine->setZValue(100);
```

### 20.1 这表示什么

辅助线需要显示在几乎所有普通图层之上，否则可能被叠加层遮住。

### 20.2 为什么普通图层不是这个值

后面 `MainWindow::updateZOrder()` 给普通 overlay 的 Z 值通常是：

```cpp
10 + i
```

见 `mainwindow.cpp:333`。

所以把辅助线设成 100 就能明显高于普通图层。

这是一种简单有效的层级约定。

---

## 21. 为什么吸附时要触发蜂鸣反馈

在 `ResizablePixmapItem.cpp:52`：

```cpp
void ResizablePixmapItem::triggerFeedback() { QApplication::beep(); }
```

并在吸附状态刚发生时调用：

```cpp
if ((showV && !m_snappedX) || (showH && !m_snappedY)) triggerFeedback();
```

### 21.1 这说明作者在关注“操作手感”

吸附不是只要算对就够了，还要让用户感知到：
- 刚刚发生了对齐

视觉上有辅助线，听觉上有蜂鸣，交互反馈更明确。

### 21.2 为什么要用 `m_snappedX / m_snappedY`

为了避免鼠标停在吸附区附近时持续疯狂响铃。

逻辑是：
- 只有从“未吸附”进入“吸附”状态时才触发一次
- 保持在吸附状态中不重复提示

这是一种很典型的“边界触发”设计。

---

## 22. 为什么移动时还要再次做边界裁剪

在 `ResizablePixmapItem.cpp:132`-`135`：

```cpp
if (targetX < sceneRect.left()) targetX = sceneRect.left();
if (targetY < sceneRect.top()) targetY = sceneRect.top();
if (targetX + w > sceneRect.right()) targetX = sceneRect.right() - w;
if (targetY + h > sceneRect.bottom()) targetY = sceneRect.bottom() - h;
```

### 22.1 这和吸附是两个不同层次

- 吸附：如果接近某个参考线，就主动对齐
- 裁剪：无论是否吸附，都不能越界

### 22.2 为什么两者都要有

如果只有吸附没有裁剪：
- 用户仍能把图层拖出画布

如果只有裁剪没有吸附：
- 图层能留在画布里，但没有对齐体验

所以这是两层不同目标：
- 合法性
- 易用性

---

## 23. `ResizableTextItem` 为什么继承 `ResizablePixmapItem`

在 `ResizableTextItem.h:76`：

```cpp
class ResizableTextItem : public ResizablePixmapItem
```

### 23.1 为什么文字层不直接写成另一套 item

因为从场景交互角度看，文字层和图片层其实极其相似：
- 都能选中
- 都能移动
- 都能缩放
- 都要同步几何
- 都要参与 Z 轴排序

不同的只是“内容生成方式”：
- 图片层内容来自现成图片
- 文字层内容来自文本渲染出来的 pixmap

所以继承是非常自然的选择。

### 23.2 这样做最大的好处

作者只需要额外解决：
- 文字如何生成 pixmap
- 双击如何改文字样式

而不用重复写移动/缩放/吸附逻辑。

这就是良好复用。

---

## 24. `ResizableTextItem` 的本质其实是“文字转图片”

在 `ResizableTextItem.h:109` 的 `regeneratePixmap()` 中：

```cpp
QFontMetrics fm(m_font);
QRect r = fm.boundingRect(m_text);
...
QPixmap pix(r.width() + padding*2, r.height() + padding*2);
...
p.drawText(..., m_text);
setPixmap(pix);
```

### 24.1 这说明文字层并不是实时绘制复杂富文本对象

而是：
- 根据当前文字配置
- 先渲染成一张透明背景 pixmap
- 再把它当普通图层使用

### 24.2 为什么这是个好选择

这样一来，文字层就能完全复用图片层机制：
- 显示方式一样
- 缩放方式一样
- 后端传输也一样

对这个项目来说，这比引入单独文字渲染管线简单很多。

---

## 25. 为什么文字层支持双击编辑

在 `ResizableTextItem.h:91`：

```cpp
void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override
```

双击后会弹出 `TextConfigDialog`，允许修改：
- 内容
- 字体
- 颜色
- 阴影

### 25.1 为什么双击是合理入口

因为在图层编辑器里，双击通常就表示：
- 进入该对象的“内容编辑/属性编辑”

这和拖动、缩放这些几何操作形成区分：
- 单击/拖动：改位置和大小
- 双击：改内容和样式

### 25.2 为什么改完后要发 `contentChanged`

在 `ResizableTextItem.h:100`：

```cpp
emit contentChanged(getId(), getPixmap().toImage());
```

因为前端文字图片变了，后端合成纹理也必须更新。

这和 `geometryChanged` 一样，都是前后端同步的一部分，只不过同步的是：
- 内容像素
而不是
- 几何尺寸

---

## 26. `MainWindow::onBtnAddSourceClicked()` 是图层系统入口

在 `mainwindow.cpp:220`：

```cpp
void MainWindow::onBtnAddSourceClicked()
```

这个函数是整个图层系统最重要的入口之一。

它会根据用户选择添加：
- 图片
- 文字
- 摄像头

### 26.1 添加图片时做了什么

在 `mainwindow.cpp:227`-`246`：
- 加载图片
- 分配 id
- 创建 `ResizablePixmapItem`
- 加入 scene
- 加入 `m_overlayItems`
- 加入图层列表
- 连接 `geometryChanged`
- 立即把几何与像素同步到后端

### 26.2 为什么“立即同步”很关键

作者特别写了注释：

```cpp
// 创建时必须立即发送几何信息和像素数据
```

原因是：
- scene 里新图层出现了
- 后端也必须立刻知道它存在
- 否则预览层和实际合成层可能不一致

这说明当前项目并不是“UI 自己玩自己的”，而是 UI 与渲染后端紧密联动。

---

## 27. 摄像头图层为什么先用占位图

在 `mainwindow.cpp:284`-`304` 的 `onCameraAdded()` 里，作者先画了一个 placeholder：

```cpp
QPixmap placeholder(rect.size());
...
p.drawRect(...);
p.drawText(..., "CAMERA " + QString::number(id));
```

然后再创建：

```cpp
ResizablePixmapItem *item = new ResizablePixmapItem(placeholder, id);
```

### 27.1 为什么不用等真实摄像头帧到了再创建

因为摄像头是异步添加的。

如果等真实帧到了才出现图层：
- UI 反馈会延迟
- 用户不知道摄像头层是否已经成功添加

### 27.2 占位图的价值

它能立刻告诉用户：
- 这个图层已经存在
- 可以先拖位置、调层级
- 后续真实画面会替换内容

这是很好的交互体验设计。

---

## 28. 为什么图层列表和场景对象要双向同步

主窗口里有两个世界：

1. 场景中的 `QGraphicsItem`
2. 右侧图层列表 `QListWidget`

作者通过：
- `addLayerToList()`
- `onLayerSelectionChanged()`
- `updateZOrder()`
- 上移/下移/删除按钮

把二者同步起来。

### 28.1 为什么不能只保留画布，不要列表

因为画布只能直观编辑位置和大小，
但很难高效管理：
- 谁在最上层
- 谁在最下层
- 某个图层的精确删除

### 28.2 为什么不能只保留列表，不要画布

因为用户看不到：
- 实际构图
- 实际叠加效果
- 实际大小和位置

所以这两个视图分别承担：
- 列表：结构管理
- 画布：空间编辑

这就是图层系统的典型双视图设计。

---

## 29. `updateZOrder()` 为什么是整个层级系统的核心

在 `mainwindow.cpp:316`：

```cpp
void MainWindow::updateZOrder()
```

这个函数做了三件事：

1. 读取右侧图层列表顺序
2. 转换成场景的 `ZValue`
3. 转换成后端渲染顺序并发出去

### 29.1 为什么要倒序遍历列表

代码注释写得很清楚：
- UI 列表 index 0 是最上面的元素
- 场景 `ZValue` 越大越在上面
- 后端渲染顺序往往是后画的覆盖前画的

所以作者倒序遍历列表，把“从底到顶”的顺序整理出来。

### 29.2 这说明什么

这里实际上存在三套层级语义：
- 列表语义
- 场景语义
- 后端渲染语义

`updateZOrder()` 的作用就是把三者统一起来。

这是这一课最值得理解的工程点之一。

---

## 30. 为什么 UI 顺序不能直接等于后端顺序

很多初学者会以为：
- 列表第一个就是先画
- 或者列表第一个就是最后画

但实际上每个系统的“顺序语义”不一样。

### 30.1 当前项目里的差异

- UI 列表：上面那项视觉上代表“最顶层”
- `QGraphicsScene`：`ZValue` 越大越顶层
- 后端渲染：通常后绘制的覆盖前绘制的

### 30.2 所以必须有一个转换层

当前项目把这个转换集中在 `updateZOrder()`，这很好，因为：
- 规则只维护一处
- 不容易前后端各自理解错顺序
- 上移/下移/删除后都能统一刷新

这就是良好的同步点设计。

---

## 31. 上移/下移/删除为什么只是改列表，再统一刷新

在 `mainwindow.cpp:341`-`379`：

- `onBtnLayerUpClicked()`
- `onBtnLayerDownClicked()`
- `onBtnLayerDelClicked()`

这些函数都遵循一个共同模式：

1. 改列表顺序或删除项
2. 再调用 `updateZOrder()`

### 31.1 这样做的好处

它避免了多处分别维护：
- scene z 值
- backend render order
- list 顺序

如果每个按钮函数都自己改三套状态，很容易不一致。

### 31.2 当前实现的优点

把“列表是唯一真源”这一点立住了：
- 列表顺序变了
- 其他表示都从列表重新推导

这种单一真源思想非常值得学习。

---

## 32. 选中同步是怎么做的

在 `mainwindow.cpp:382`：

```cpp
void MainWindow::onLayerSelectionChanged()
```

逻辑是：
- 先把所有 overlay item 设为未选中
- 再读取列表当前选中项
- 找到对应 id
- 把场景 item 设为选中

### 32.1 为什么作者选择“列表驱动场景选择”

因为右侧列表是图层管理主界面的一部分。

用户点列表项时，希望：
- 场景中对应对象被高亮
- 能看到选中框和缩放手柄

### 32.2 当前实现的一个特点

它明显更强调：
- 从列表管理图层

而不是完整双向同步“点击场景 item 自动选中列表项”。

这说明当前项目图层编辑功能已经够用，但仍然有继续增强空间。

---

## 33. `updatePreview()` 为什么还要负责图层自适应

在 `mainwindow.cpp:394`：

```cpp
void MainWindow::updatePreview(QImage img)
```

这本来像是背景预览更新函数，但里面还做了一件很重要的事：
- 当场景尺寸变化时，按比例调整所有 overlay item 的位置和大小

### 33.1 为什么场景尺寸会变化

因为主预览帧分辨率可能变化，例如：
- 切换显示器
- 切换采集源
- 采集尺寸调整

### 33.2 如果不做这层适配会怎样

那么原来的叠加层：
- 位置可能跑偏
- 大小比例可能不合理
- 摄像头框和文本框会失去原本构图关系

### 33.3 当前实现怎么做

在 `mainwindow.cpp:399`-`412`：

```cpp
qreal scaleX = (qreal) img.width() / m_lastSceneSize.width();
qreal scaleY = (qreal) img.height() / m_lastSceneSize.height();
...
item->setPos(newX, newY);
item->setRect(QRectF(0, 0, newW, newH));
EventBus::instance()->sendOverlayGeometryChanged(...);
```

也就是：
- 位置按比例缩放
- 尺寸也按比例缩放
- 然后把新几何再同步给后端

这说明图层系统不只是静态编辑器，而是：

> **会随着底层画布分辨率变化而保持相对布局稳定。**

---

## 34. 为什么 `m_lastSceneSize` 很重要

在 `mainwindow.h:114`：

```cpp
QSize m_lastSceneSize;
```

它的作用就是记录上一次场景尺寸，用于下一帧做相对缩放。

### 34.1 为什么不能只根据当前尺寸处理

因为要做比例变换，你必须知道：
- 从哪个旧尺寸变到哪个新尺寸

否则就无法算：
- `scaleX`
- `scaleY`

### 34.2 这体现了什么设计思想

图层位置不是“绝对随意值”，而是和场景分辨率有关系的相对布局。

所以项目需要保存上一轮场景状态，才能稳定迁移到新场景。

---

## 35. 为什么 `sendOverlayGeometryChanged()` 和 `fireOverlayUpdate()` 要分开

从 `mainwindow.cpp` 的添加与更新逻辑可见，作者把同步分成两类：

### 35.1 几何同步

```cpp
sendOverlayGeometryChanged(id, QRect(...))
```

表示：
- 位置
- 宽高

### 35.2 内容同步

```cpp
fireOverlayUpdate(id, img)
```

表示：
- 这个图层的像素内容变了

### 35.3 为什么要拆开

因为很多时候只会变一类：
- 拖动图层：只变几何
- 改文字内容：只变内容
- 预览分辨率变了：几何可能变，但内容未必变
- 摄像头新帧到了：内容变，但位置不变

拆开以后：
- 同步更精确
- 后端处理更清晰
- 不需要每次都做全量更新

---

## 36. 从工程角度重新理解第13课

这一课最核心的不是学会几个 Qt 类名，而是理解下面这件事：

> **图层系统本质上是在维护“对象内容、对象几何、对象层级、对象交互、后端同步”这五件事的一致性。**

在这个项目里：
- `QGraphicsScene` 提供场景组织能力
- `ResizablePixmapItem` 提供统一图层交互能力
- `ResizableTextItem` 复用图片图层机制处理文字层
- `QListWidget` 提供结构化图层管理视图
- `EventBus` 把前端图层编辑同步到后端渲染系统

所以它已经不只是“预览区”，而是一个小型可视化合成编辑器。

这也是为什么很多录屏/直播软件的 UI 难点，不在“把画面显示出来”，而在：
- 如何让用户自然地摆放这些元素
- 如何保证显示层和真正输出层始终一致

而当前项目在这方面已经有很明确的工程思路。

---

## 37. 本课应重点掌握的代码片段

### 37.1 创建场景和视图

```cpp
m_scene = new QGraphicsScene(this);
m_view = new QGraphicsView(m_scene);
m_bgItem = new QGraphicsPixmapItem(defaultBg);
m_scene->addItem(m_bgItem);
```

### 37.2 图层 item 基础能力

```cpp
setAcceptHoverEvents(true);
setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
```

### 37.3 内容与几何分离

```cpp
m_pixmap = pixmap;
// 不重置 m_rect，保持当前的大小和位置，只更新内容
update();
```

### 37.4 选中态绘制

```cpp
if (isSelected()) {
    painter->drawRect(m_rect);
    painter->drawRect(m_rect.right() - m_handleSize, m_rect.bottom() - m_handleSize, m_handleSize, m_handleSize);
}
```

### 37.5 缩放逻辑

```cpp
QPointF diff = event->pos() - m_startPos;
qreal newW = qMax(50.0, m_initialRect.width() + diff.x());
qreal newH = qMax(50.0, m_initialRect.height() + diff.y());
```

### 37.6 吸附到场景中心/边缘

```cpp
if (std::abs((x + w/2) - sceneRect.center().x()) < SNAP_THRESHOLD) { ... }
else if (std::abs(x - sceneRect.left()) < SNAP_THRESHOLD) { ... }
else if (std::abs((x + w) - sceneRect.right()) < SNAP_THRESHOLD) { ... }
```

### 37.7 图层顺序同步

```cpp
m_overlayItems[id]->setZValue(10 + i);
renderOrder.append(id);
EventBus::instance()->sendCommandUpdateRenderOrder(renderOrder);
```

### 37.8 场景分辨率变化时按比例调整图层

```cpp
qreal scaleX = (qreal) img.width() / m_lastSceneSize.width();
qreal scaleY = (qreal) img.height() / m_lastSceneSize.height();
item->setPos(newX, newY);
item->setRect(QRectF(0, 0, newW, newH));
```

---

## 38. 本课作业

1. **框架选择题**：解释为什么项目的预览叠加区更适合用 `QGraphicsScene + QGraphicsView`，而不是一个普通 `QLabel` 或一堆独立 `QWidget`。

2. **抽象设计题**：说明为什么 `ResizableTextItem` 要继承 `ResizablePixmapItem`。这背后复用了哪些交互能力？

3. **交互逻辑题**：解释 `mousePressEvent()`、`mouseMoveEvent()`、`itemChange()` 在这个图层系统里分别负责什么。为什么拖动吸附逻辑放在 `itemChange()` 中更合适？

4. **层级同步题**：说明 `updateZOrder()` 为什么要同时更新 Scene 的 `ZValue` 和后端 `renderOrder`。如果只更新其中一边，会出现什么现象？

5. **分辨率适配题**：解释 `updatePreview()` 中 `scaleX / scaleY` 的作用。为什么主预览尺寸变化后，还要同步调整每个 overlay 的位置与大小？

---

*上一课：[第12课：Qt无边框窗口与自定义控件](./第12课：Qt无边框窗口与自定义控件.md)*  
*下一课：[第14课：日志、崩溃捕获与调试](./第14课：日志、崩溃捕获与调试.md)*
