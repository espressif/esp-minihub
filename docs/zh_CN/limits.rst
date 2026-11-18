固定上限
========

:link_to_translation:`en:[English]`

本文汇总 Host 的各项容量上限。每行 ``Defined at`` 列给出代码中的定义位置；修改上限
须编辑对应文件中的符号。

.. minihub-limits:: core

.. minihub-limits:: rules

.. minihub-limits:: scenes

.. minihub-limits:: store

.. minihub-limits:: web

.. minihub-limits:: auth

.. minihub-limits:: matter

.. minihub-limits:: radios

.. minihub-limits:: update

.. minihub-limits:: JS

.. minihub-limits:: hub

如何修改一个上限
----------------

``Defined at`` 一栏指向两类位置，改法不同：

* 指向 ``Kconfig.minihub`` 的行是 **缺省值**，可以在产品里覆盖，不必改 Core 源码：
  在产品的 Kconfig 片段或 ``sdkconfig.defaults`` 中给对应符号重新赋值，重新构建即可生效。
* 指向 ``.h`` 里的 ``#define``、或 ``.c`` 里 ``char buf[N]`` 的行是 **编译期常量**，
  改源码后必须重新构建固件；它们多数直接决定缓冲区或任务栈的大小。

改动时要注意
------------

* 这些上限彼此咬合，不要只改一个。例如点位注册表上限 64 是整台 Hub 最紧的天花板，
  Matter 把每个 endpoint 都算一条点，所以一台多端点设备会占掉好几行；把 Matter 表
  调大之前，先确认 64 条点还够用（见 :doc:`matter` ）。
* 缓冲区常量（如各类 ``*_BODY_MAX``、``char buf[N]``）是按任务栈大小裁好的。调大它们，
  往往要同时调大对应的 ``*_STACK_SIZE`` Kconfig，否则任务栈会溢出。JS 的 worker 栈、
  Hub 重载栈都走这条规则。
* 文档表格里同样列着这些数值；代码里的常量改了，要同步更新文档，否则文档构建会失败。
* 这些常量大多写在共享的 Core 组件里，改动会影响所有基于同一份 Core 构建的产品。
  能走产品级 Kconfig 覆盖的就不要改 Core 源码。

风险
----

* 只调大计数、不调配套的缓冲区，会出现静默截断：事件队列积压到 32 条后被丢弃、超过导入
  上限的文档整份被拒、OTA 分块被截。
* 改 ``Kconfig.minihub`` 之外的编译期常量属于改 Core，会牵动所有产品；优先考虑产品级覆盖。
* Matter 表调大会多吃 PSRAM 并拉长配网；点位数和各类缓冲上调会吃 heap，Flash 小的料上
  ``storage`` 与 ``modstore`` 两个可写分区可能放不下（见 :doc:`production` ）。

相关： :doc:`data_model` （点位）、 :doc:`rules` （编写）、 :doc:`production`
（分区与烧录）、 :doc:`api/host_rest` （请求体）。
