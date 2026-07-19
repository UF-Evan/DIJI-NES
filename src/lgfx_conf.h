#include <LovyanGFX.hpp>

// 使用LovyanGFX对ESP32进行配置的例子

/// 为你自己的配置创建一个类，派生自LGFX_Device。
class LGFX : public lgfx::LGFX_Device
{
/*
 该类的名称可以从 "LGFX "改为其他名称。
 当与AUTODETECT一起使用时，"LGFX "已经被使用了，所以要把名字改为LGFX以外的名字。
 如果同时使用多个面板，给每个面板一个不同的名字。
 如果改变了类的名称，构造函数的名称也必须改成相同的名称。

 命名方案可以自由决定，但万一配置的数量增加，请将构造函数的名称改为与面板的名称相同。
 例如，如果你在ESP32 DevKit-C中设置了一个ILI9341 SPI连接，你可以使用
  LGFX_DevKitC_SPI_ILI9341
 并将文件名与类名相匹配，这样在使用时就不容易混淆了。
//*/

// 为要连接的屏幕类型准备一个实例。
//lgfx::Panel_GC9A01      _panel_instance;
//lgfx::Panel_GDEW0154M09 _panel_instance;
//lgfx::Panel_HX8357B     _panel_instance;
//lgfx::Panel_HX8357D     _panel_instance;
//lgfx::Panel_ILI9163     _panel_instance;
//  lgfx::Panel_ILI9341     _panel_instance;
//lgfx::Panel_ILI9342     _panel_instance;
//lgfx::Panel_ILI9481     _panel_instance;
//lgfx::Panel_ILI9486     _panel_instance;
//lgfx::Panel_ILI9488     _panel_instance;
//lgfx::Panel_IT8951      _panel_instance;
//lgfx::Panel_RA8875      _panel_instance;
//lgfx::Panel_SH110x      _panel_instance; // SH1106, SH1107
//lgfx::Panel_SSD1306     _panel_instance;
//lgfx::Panel_SSD1327     _panel_instance;
//lgfx::Panel_SSD1331     _panel_instance;
//lgfx::Panel_SSD1351     _panel_instance; // SSD1351, SSD1357
//lgfx::Panel_SSD1963     _panel_instance;
//lgfx::Panel_ST7735      _panel_instance;
//lgfx::Panel_ST7735S     _panel_instance;
lgfx::Panel_ST7789      _panel_instance;
//lgfx::Panel_ST7796      _panel_instance;

// 为屏幕所连接的总线类型准备一个实例。
  lgfx::Bus_SPI        _bus_instance;   // SPI
//lgfx::Bus_I2C        _bus_instance;   // I2C
//lgfx::Bus_Parallel8  _bus_instance;   // 8 Parallel

// 如果进行背光控制，请提供一个实例。 (如果不需要则删除）。
// lgfx::Light_PWM     _light_instance;

// 为触摸屏类型准备一个实例。 (如果不需要则删除）。
// lgfx::Touch_FT5x06           _touch_instance; // FT5206, FT5306, FT5406, FT6206, FT6236, FT6336, FT6436
//lgfx::Touch_GSL1680E_800x480 _touch_instance; // GSL_1680E, 1688E, 2681B, 2682B
//lgfx::Touch_GSL1680F_800x480 _touch_instance;
//lgfx::Touch_GSL1680F_480x272 _touch_instance;
//lgfx::Touch_GSLx680_320x320  _touch_instance;
//lgfx::Touch_GT911            _touch_instance;
//lgfx::Touch_STMPE610         _touch_instance;
//lgfx::Touch_TT21xxx          _touch_instance; // TT21100
//lgfx::Touch_XPT2046          _touch_instance;

public:

  // 创建一个构造函数并在这里配置各种设置。
  // 如果你改变了类的名称，请为构造函数指定相同的名称。
  LGFX(void)
  {
    { // 配置总线控制设置。
      auto cfg = _bus_instance.config();   // 获得总线配置的结构。

// SPI设定
      cfg.spi_host = SPI3_HOST;     // 选择要使用的SPI ESP32-S2,C3 : SPI2_HOST 或 SPI3_HOST / ESP32 : VSPI_HOST 或 HSPI_HOST
       // * 随着ESP-IDF版本的升级，VSPI_HOST , HSPI_HOST的描述被废弃了，所以如果发生错误，请使用SPI2_HOST , 
      cfg.spi_mode = 0;             //设置SPI通信模式(0 ~ 3)
      cfg.freq_write = 80000000;    // 发送时的SPI时钟（最大80MHz，四舍五入为80MHz的整数）。
      cfg.freq_read  = 6000000;    // 接收时的SPI时钟
      cfg.spi_3wire  = true;        // 如果用MOSI引脚进行接收，则设置为true
      cfg.use_lock   = false;        //如果使用交易锁则设置为true
      cfg.dma_channel = SPI_DMA_CH_AUTO; // 设置要使用的DMA通道（0=不使用DMA/1=1ch/2=ch/SPI_DMA_CH_AUTO=auto设置）。
      // *随着ESP-IDF版本的升级，现在推荐使用SPI_DMA_CH_AUTO（自动设置）作为DMA通道，1ch和2ch被弃用。
      cfg.pin_sclk = 14;            // 设置SPI SCLK引脚编号
      cfg.pin_mosi = 13;            // 设置SPI的MOSI引脚编号
      cfg.pin_miso = -1;            // 设置SPI的MISO针脚编号（-1 = 禁用）。
      cfg.pin_dc   = 11;            // 设置SPI的D/C针脚编号（-1 = 禁用）。
     // 当与SD卡共同使用SPI总线时，必须无遗漏地设置MISO。

      _bus_instance.config(cfg);    // //反映总线上的配置值。
      _panel_instance.setBus(&_bus_instance);      /// 设置屏幕总线。
    }

    { // 配置显示面板控制设置。
      auto cfg = _panel_instance.config();    // 获取屏幕配置的结构。。

      cfg.pin_cs           =    10;  // 连接CS的引脚编号（-1 = 禁用）。
      cfg.pin_rst          =    12;  // 连接RST的引脚编号 (-1 = 禁用)
      cfg.pin_busy         =    -1;  // 连接BUSY的引脚编号 (-1 = 禁用)

        // * 下面的设置对每个面板都有一般的默认值，如果你对某个项目不确定，可以把它注释出来并试一试。

      cfg.panel_width      =   240;  // 实际可显示的宽度
      cfg.panel_height     =   320;  // 实际可显示的高度
      cfg.offset_x         =     0;  // 在屏幕的X方向上的偏移量
      cfg.offset_y         =     0;  // 在屏幕的Y方向上的偏移量
      cfg.offset_rotation  =     2;  // 旋转方向的偏移量为0~7（4~7为倒置）。
      cfg.dummy_read_pixel =     8;  // 读取像素前的假读位数量
      cfg.dummy_read_bits  =     1;  // 读取非像素数据前的虚拟读取位数
      cfg.readable         =  true;  // 如果可以读取数据，则设置为true。
      cfg.invert           = false;   // 设定 是否反色，有些屏幕需要设置这个值才能获取正确的颜色
      cfg.rgb_order        = false;  // true 为 RGB false 为 BGR
      cfg.dlen_16bit       = false;  // 如果面板在16位并行或SPI中以16位单位传输数据长度，则设置为true。
      cfg.bus_shared       = false;  // SDカー如果与SD卡共享总线，则设置为true（总线控制由drawJpgFile等执行）。

      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance); // 设置要使用的面板。
  }
};