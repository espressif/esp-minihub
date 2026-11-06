版本
====

:link_to_translation:`en:[English]`

本指南对应 |product_version| —— 就是设备自己上报的那个字符串。把它和
``GET /api/system`` 里的 commit 一起读：版本说的是产品作者声明的版本，commit 说的是
被编译的那棵树。

一个设备一个号，两条发布线
--------------------------

设备版本是 ``MINIHUB_VERSION`` ，在 :file:`cmake/minihub_bundle/Kconfig.projbuild`
里设一次（缺省 ``0.1.0`` ）。同一个字符串就是固件上报的 ``firmware_version`` ，
是 ``tools/deploy.sh`` 拿来命名 ``Firmwares/<chip>/<version>/`` 目录的那个号，也是
控制台侧栏显示的内容。它不是 ``git describe`` ：commit 单独上报，所以「版本号没动的
重新编译」会表现为同一版本下的另一个 commit。

控制台走自己的发布线。``MINIHUB_WEB_VERSION`` （ :file:`components/minihub_web/Kconfig.minihub` ）
是它的**唯一**来源：固件构建和发布都从它派生，两边永远一致。填写时不带前面的 ``v``
（只有部署目录名会加）。它的缺省值跟着 ``MINIHUB_VERSION`` ，但两者刻意分开：只改前端的
修复抬高它就够，不必动固件。源码头里没有、也别手写 ``version.txt`` —— 它是构建时生成的。

所以 **一台 Hub 合理地上报两个版本** ，分两步升级过的设备通常就是如此。拿设备跟本指南
对比之前，两个都看：

.. code-block:: bash

   curl -s -H "Authorization: Bearer $MINIHUB_TOKEN" http://HUB/api/system \
     | python3 -m JSON.tool   # version、commit
   # 控制台自己的值是 window.mhWebVersion ，侧栏也会显示

固件比控制台新是正常的；控制台比固件新，通常说明 ``app-flash`` 只换了应用，
``/data/web`` 还是上一代。把控制台当成单独一步升级 （ :doc:`web_console` ），或者
整机烧录 （ :doc:`production` ）。

升级能给你什么
--------------

部署树是否给出某个固件，由服务器拿设备上报的 ``cur_version`` 与它自己发布的目录名
比较决定，所以 **版本字符串必须能单调排序** 。纯文本排序下 ``0.1.10`` 会排在
``0.1.9`` 前面，这一条就足以让设备以为自己已经是最新：请零填充，或者换成能正确比较的
编号方案。商店包按目录里自己的版本字段比较，而 ``POST /api/store/upgrade`` 不接受
降级 —— 要推同一个或更旧的构建请用 ``reinstall`` （ :doc:`api/plugin_rest` ）。

.. only:: not hide_l3

   原生模块比对的不是产品版本，而是 Host ABI：``module.json`` 里的 ``abi_version`` 与
   ``core_api_*`` 必须把 Host 的 ``MINIHUB_MODULE_ABI_VERSION`` 夹在中间
   （ :doc:`api/module_abi` ）。只有当老 Host 再也不能安全加载插件时，ABI 号才会变。

发布说明在哪
------------

没有汇总式的 changelog。每个发布出去的固件自带
``Firmwares/<chip>/<version>/release_notes.txt`` （ ``tools/publish_firmware.py``
把文件写在固件旁边），Hub 会在升级界面和 ``GET /api/update/notes`` 里显示被给出的那一份
（ :doc:`ota` ）。需要 changelog 的产品要自己按版本把这些文件拼起来 —— 除此之外没有
别处记录两次构建之间改了什么。

钉住的工具链
------------

本指南只钉一个 ESP-IDF 修订与一个目标芯片，都写在 :doc:`getting_started` 。换钉不是
补丁说明：它会改动构建步骤、分区假设，有时还有内存预算，所以对你自己的移植来说，
「指南换到新的 IDF」就是一次版本分界。

相关： :doc:`getting_started`、 :doc:`production`、 :doc:`ota`、 :doc:`web_console`、
:doc:`api/deploy_index`、 :doc:`limits` 。
