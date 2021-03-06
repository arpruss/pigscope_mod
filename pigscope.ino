/*.
(c) Andrew Hull - 2015
modifications (c) Alexander Pruss - 2018

STM32-O-Scope - aka "The Pig Scope" or pigScope released under the GNU GENERAL PUBLIC LICENSE Version 2, June 1991

https://github.com/pingumacpenguin/STM32-O-Scope

Adafruit Libraries released under their specific licenses Copyright (c) 2013 Adafruit Industries.  All rights reserved.

*/

#include <libmaple/dma.h>
#include <VectorDisplay.h>

#define PORTRAIT 0
#define LANDSCAPE 1
#undef SUPPORT_TIME

/* For reference on STM32F103CXXX

variants/generic_stm32f103c/board/board.h:#define BOARD_NR_SPI              2
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI1_NSS_PIN        PA4
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI1_MOSI_PIN       PA7
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI1_MISO_PIN       PA6
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI1_SCK_PIN        PA5

variants/generic_stm32f103c/board/board.h:#define BOARD_SPI2_NSS_PIN        PB12
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI2_MOSI_PIN       PB15
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI2_MISO_PIN       PB14
variants/generic_stm32f103c/board/board.h:#define BOARD_SPI2_SCK_PIN        PB13

*/

#define TEST_WAVE_PIN       PB1     //PB1 PWM 500 Hz 

SerialDisplayClass TFT;
VectorDisplayMessage msg;

// LED - blinks on trigger events - leave this undefined if your board has no controllable LED
// define as PC13 on the "Red/Blue Pill" boards and PD2 on the "Yellow Pill R"
#define BOARD_LED PB12 //PB0

// Display colours
#define BEAM1_COLOUR 0x07E0 // ILI9341_GREEN
#define BEAM2_COLOUR 0xF800 // ILI9341_RED
#define GRATICULE_COLOUR 0x07FF
#define BEAM_OFF_COLOUR 0x0000 //ILI9341_BLACK
#define CURSOR_COLOUR 0x07E0 // ILI9341_GREEN

// Analog input
#define ANALOG_MAX_VALUE 4096
const int8_t analogInPin = PB0;   // Analog input pin: any of LQFP44 pins (PORT_PIN), 10 (PA0), 11 (PA1), 12 (PA2), 13 (PA3), 14 (PA4), 15 (PA5), 16 (PA6), 17 (PA7), 18 (PB0), 19  (PB1)
float samplingTime = 0;
float displayTime = 0;


// Variables for the beam position
uint16_t signalX ;
uint16_t signalY ;
uint16_t signalY1;
//int16_t xZoomFactor = 1;
// yZoomFactor (percentage)
int16_t yZoomFactor = 100; //Adjusted to get 3.3V wave to fit on screen
int16_t yPosition = 0 ;

// Startup with sweep hold off or on
boolean triggerHeld = false;


unsigned long sweepDelayFactor = 1;
unsigned long timeBase = 200;  //Timebase in microseconds

// Screen dimensions
int16_t myWidth ;
int16_t myHeight ;

//Trigger stuff
boolean notTriggered ;

// Sensitivity is the necessary change in AD value which will cause the scope to trigger.
// If VAD=3.3 volts, then 1 unit of sensitivity is around 0.8mV but this assumes no external attenuator. Calibration is needed to match this with the magnitude of the input signal.

// Trigger is setup in one of 32 positions
#define TRIGGER_POSITION_STEP ANALOG_MAX_VALUE/32
// Trigger default position (half of full scale)
int32_t triggerValue = 2048; 

int16_t retriggerDelay = 0;
int8_t triggerType = 4; //1-both 2-negative 3-positive 4-continuous

//Array for trigger points
uint16_t triggerPoints[2];


// Serial output of samples - off by default. Toggled from UI/Serial commands.
boolean serialOutput = false;

float fps=0;

// Create serial port
class IgnorePort : public Print {
  public:
    virtual size_t write(uint8_t c) {return 0;}
    void begin(unsigned baud) {}
} serial_debug;

// Samples - depends on available RAM 6K is about the limit on an STM32F103C8T6
// Bear in mind that the ILI9341 display is only able to display 240x320 pixels, at any time but we can output far more to the serial port, we effectively only show a window on our samples on the TFT.
# define maxSamples (1024*6) //1024*6
uint32_t startSample = 0; //10
uint32_t endSample = maxSamples ;

// Array for the ADC data
//uint16_t dataPoints[maxSamples];
uint32_t dataPoints32[maxSamples / 2];
uint16_t *dataPoints = (uint16_t *)&dataPoints32;

//array for computed data (speedup)
//uint16_t dataPlot[320]; //max(width,height) for this display


// End of DMA indication
volatile static bool dma1_ch1_Active;
#define ADC_CR1_FASTINT 0x70000 // Fast interleave mode DUAL MODE bits 19-16

void toggleSerial();
void toggleHold();
void decreaseTimebase();
void increaseTimebase();
void decreaseZoomFactor();
void increaseZoomFactor();
void scrollRight();
void scrollLeft();
void incEdgeType();
void decreaseYposition();
void increaseYposition();
void decreaseTriggerPosition();
void increaseTriggerPosition();
void toggleTestPulseOn();
void toggleTestPulseOff();

struct {
  char  c;
  const char* label;
  void (*function)();
} commands[] = {
//  { 's', "Serial", toggleSerial },                   // Turns serial sample output on/off
  { 'h', "Hold", toggleHold },                       // Turns triggering on/off
  { 't', "T-", decreaseTimebase },                // decrease Timebase by 10x
  { 'T', "T+", increaseTimebase },                // increase Timebase by 10x
  { 'z', "Zoom-", decreaseZoomFactor },               // decrease Zoom
  { 'Z', "Zoom+", increaseZoomFactor },              // increase Zoom
//  { 'r', ">>", scrollRight },                      // start onscreen trace further right
//  { 'l', "<<", scrollLeft },                      // start onscreen trae further left
  { 'e', "Edge", incEdgeType },                     // increment the trigger edge type 0 1 2 3 0 1 2 3 etc
  { 'y', "Y-", decreaseYposition },               // move trace Down
  { 'Y', "Y+", increaseYposition },               // move trace Up
  { 'g', "Trig-", decreaseTriggerPosition },         // move trigger position Down
  { 'G', "Trig+", increaseTriggerPosition },         // move trigger position Up
  { 'P', "Pulse1", toggleTestPulseOn },               // Toggle the test pulse pin from high impedence input to square wave output.
  { 'p', "Pulse0", toggleTestPulseOff }               // Toggle the Test pin from square wave test to high impedence input.
};

void setup()
{
  // BOARD_LED blinks on triggering assuming you have an LED on your board. If not simply dont't define it at the start of the sketch.
#if defined BOARD_LED
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, HIGH);
  delay(1000);
  digitalWrite(BOARD_LED, LOW);
  delay(1000);
#endif
//  pinMode(TFT_LED, OUTPUT);
//  digitalWrite(TFT_LED, HIGH);

  serial_debug.begin(115200);
  adc_calibrate(ADC1);
  adc_calibrate(ADC2);
  setADCs (); //Setup ADC peripherals for interleaved continuous mode.

  //
  // Serial command setup
  // Setup callbacks for SerialCommand commands

  TFT.begin();
  TFT.continuousUpdate(false);
  TFT.toast("Pigscope");

  for(unsigned i=0;i<sizeof(commands)/sizeof(*commands);i++)
    TFT.addButton(commands[i].c, commands[i].label);

  // The test pulse is a square wave of approx 3.3V (i.e. the STM32 supply voltage) at approx 1 kHz
  // "The Arduino has a fixed PWM frequency of 490Hz" - and it appears that this is also true of the STM32F103 using the current STM32F03 libraries as per
  // STM32, Maple and Maple mini port to IDE 1.5.x - http://forum.arduino.cc/index.php?topic=265904.2520
  // therefore if we want a precise test frequency we can't just use the default uncooked 50% duty cycle PWM output.
  timer_set_period(Timer3, 1000);
  toggleTestPulseOn();

  // Set up our sensor pin(s)
  pinMode(analogInPin, INPUT_ANALOG);

  // initialize the display
  clearTFT();
  myHeight   = 320;
  myWidth  = 240;
  TFT.coordinates(myHeight, myWidth);
  //TFT.setRotation(PORTRAIT);
  TFT.setTextColor(CURSOR_COLOUR, BEAM_OFF_COLOUR) ;
#if defined TOUCH_SCREEN_AVAILABLE
  touchCalibrate();
#endif

  //TFT.setRotation(LANDSCAPE);
  clearTFT();
//  showGraticule();
  showCredits(); // Honourable mentions ;¬)
  TFT.update();
  delay(1000) ; //5000
  clearTFT();
  notTriggered = true;
  showGraticule();
  showLabels();
  digitalWrite(PB12,1);
  
  
  clearTFT();
  TFT.setCursor(0,10);
}

bool processMessage() {
  if (TFT.readMessage(&msg) && msg.what == MESSAGE_BUTTON) {
    for(unsigned i=0;i<sizeof(commands)/sizeof(*commands);i++)
      if (msg.data.button == commands[i].c) {
        commands[i].function();
        return true;
      }
  }
  return false;
}

void loop()
{

//  if (TFT.isTouchDown()) {
//    TFT.drawPixel(TFT.getTouchX(), TFT.getTouchY(), BEAM2_COLOUR);
//  }

  samplingTime = 0;
  if ( !triggerHeld  )
  {
    // Wait for trigger
    trigger();
    if ( !notTriggered )
    {
      displayTime = micros();
      //blinkLED();

      // Take our samples
      takeSamples();
      
      //Blank  out previous plot
      TFT.fillScreen(BEAM_OFF_COLOUR);
//      TFTSamplesClear(BEAM_OFF_COLOUR);

      // Show the showGraticule
      showGraticule();

      //Display the samples
      TFTSamples(BEAM1_COLOUR);
      
      // Display the Labels ( uS/Div, Volts/Div etc).
      showLabels();
    
      TFT.update();
      displayTime = (micros() - displayTime);
      fps = 1000000./(displayTime+samplingTime);

    }else {
          showGraticule();
          TFT.update();
    }
#ifdef SUPPORT_TIME    
    // Display the RTC time.
    showTime();
#endif    
  }

  processMessage();
  // Wait before allowing a re-trigger
  if (retriggerDelay>0) 
    delay(retriggerDelay);
  // DEBUG: increment the sweepDelayFactor slowly to show the effect.
  // sweepDelayFactor ++;

}

void showGraticule()
{
  TFT.drawRect(0, 0, myHeight, myWidth, GRATICULE_COLOUR);
  // Dot grid - ten distinct divisions (9 dots) in both X and Y axis.
  for (uint16_t TicksX = 1; TicksX < 10; TicksX++)
  {
    for (uint16_t TicksY = 1; TicksY < 10; TicksY++)
    {
      TFT.drawPixel(  TicksX * (myHeight / 10), TicksY * (myWidth / 10), GRATICULE_COLOUR);
    }
  }
  // Horizontal and Vertical centre lines 5 ticks per grid square with a longer tick in line with our dots
  for (uint16_t TicksX = 0; TicksX < myWidth; TicksX += (myHeight / 50))
  {
    if (TicksX % (myWidth / 10) > 0 )
    {
      TFT.drawFastHLine(  (myHeight / 2) - 1 , TicksX, 3, GRATICULE_COLOUR);
    }
    else
    {
      TFT.drawFastHLine(  (myHeight / 2) - 3 , TicksX, 7, GRATICULE_COLOUR);
    }

  }
  for (uint16_t TicksY = 0; TicksY < myHeight; TicksY += (myHeight / 50) )
  {
    if (TicksY % (myHeight / 10) > 0 )
    {
      TFT.drawFastVLine( TicksY,  (myWidth / 2) - 1 , 3, GRATICULE_COLOUR);
    }
    else
    {
      TFT.drawFastVLine( TicksY,  (myWidth / 2) - 3 , 7, GRATICULE_COLOUR);
    }
  }
}

void setADCs ()
{
  //  const adc_dev *dev = PIN_MAP[analogInPin].adc_device;
  int pinMapADCin = PIN_MAP[analogInPin].adc_channel;
  adc_set_sample_rate(ADC1, ADC_SMPR_1_5); //=0,58uS/sample.  ADC_SMPR_13_5 = 1.08uS - use this one if Rin>10Kohm,
  adc_set_sample_rate(ADC2, ADC_SMPR_1_5);    // if not may get some sporadic noise. see datasheet.

  //  adc_reg_map *regs = dev->regs;
  adc_set_reg_seqlen(ADC1, 1);
  ADC1->regs->SQR3 = pinMapADCin;
  ADC1->regs->CR2 |= ADC_CR2_CONT; // | ADC_CR2_DMA; // Set continuous mode and DMA
  ADC1->regs->CR1 |= ADC_CR1_FASTINT; // Interleaved mode
  ADC1->regs->CR2 |= ADC_CR2_SWSTART;

  ADC2->regs->CR2 |= ADC_CR2_CONT; // ADC 2 continuos
  ADC2->regs->SQR3 = pinMapADCin;
}


// Crude triggering on positive or negative or either change from previous to current sample.
void trigger()
{
  notTriggered = true;
  switch (triggerType) {
    case 1:
      triggerNegative() ;
      break;
    case 2:
      triggerPositive() ;
      break;
    case 3:
      triggerBoth() ;
      break;
    case 4:
      notTriggered = false;
      break;
  }
}

void triggerBoth()
{
  int count = 0;
  triggerPoints[0] = analogRead(analogInPin);
  while(notTriggered){
    triggerPoints[1] = analogRead(analogInPin);
    if ( ((triggerPoints[1] < triggerValue) && (triggerPoints[0] > triggerValue)) ||
         ((triggerPoints[1] > triggerValue) && (triggerPoints[0] < triggerValue)) ){
      notTriggered = false;
    }
    triggerPoints[0] = triggerPoints[1]; //analogRead(analogInPin);
    count++;
    if ((count % 1024 == 0) && processMessage())
      return;
  }
}

void triggerPositive() {
  int count = 0;
  triggerPoints[0] = analogRead(analogInPin);
  while(notTriggered){
    triggerPoints[1] = analogRead(analogInPin);
    if ((triggerPoints[1] > triggerValue) && (triggerPoints[0] < triggerValue) ){
      notTriggered = false;
    }
    triggerPoints[0] = triggerPoints[1]; //analogRead(analogInPin);
    count++;
    if ((count % 1024 == 0) && processMessage())
      return;
  }
}

void triggerNegative() {
  int count = 0;
  triggerPoints[0] = analogRead(analogInPin);
  while(notTriggered){
    triggerPoints[1] = analogRead(analogInPin);
    if ((triggerPoints[1] < triggerValue) && (triggerPoints[0] > triggerValue) ){
      notTriggered = false;
    }
    count++;
    if ((count % 1024 == 0) && processMessage())
      return;
    triggerPoints[0] = triggerPoints[1]; //analogRead(analogInPin);
  }
}

void incEdgeType() {
  triggerType += 1;
  if (triggerType > 4)
  {
    triggerType = 1;
  }
  /*
  serial_debug.println(triggerPoints[0]);
  serial_debug.println(triggerPoints[1]);
  serial_debug.println(triggerType);
  */
}

void clearTFT()
{
  TFT.fillScreen(BEAM_OFF_COLOUR);                // Blank the display
}

void blinkLED()
{
#if defined BOARD_LED
  digitalWrite(BOARD_LED, LOW);
  delay(10);
  digitalWrite(BOARD_LED, HIGH);
#endif

}

// Grab the samples from the ADC
// Theoretically the ADC can not go any faster than this.
//
// According to specs, when using 72Mhz on the MCU main clock,the fastest ADC capture time is 1.17 uS. As we use 2 ADCs we get double the captures, so .58 uS, which is the times we get with ADC_SMPR_1_5.
// I think we have reached the speed limit of the chip, now all we can do is improve accuracy.
// See; http://stm32duino.com/viewtopic.php?f=19&t=107&p=1202#p1194

void takeSamples ()
{
  // This loop uses dual interleaved mode to get the best performance out of the ADCs
  //

  dma_init(DMA1);
  dma_attach_interrupt(DMA1, DMA_CH1, DMA1_CH1_Event);

  adc_dma_enable(ADC1);
  dma_setup_transfer(DMA1, DMA_CH1, &ADC1->regs->DR, DMA_SIZE_32BITS,
                     dataPoints32, DMA_SIZE_32BITS, (DMA_MINC_MODE | DMA_TRNS_CMPLT));// Receive buffer DMA
  dma_set_num_transfers(DMA1, DMA_CH1, maxSamples / 2);
  dma1_ch1_Active = 1;
  //  regs->CR2 |= ADC_CR2_SWSTART; //moved to setADC
  dma_enable(DMA1, DMA_CH1); // Enable the channel and start the transfer.
  //adc_calibrate(ADC1);
  //adc_calibrate(ADC2);
  samplingTime = micros();
  while (dma1_ch1_Active);
  samplingTime = (micros() - samplingTime);

  dma_disable(DMA1, DMA_CH1); //End of trasfer, disable DMA and Continuous mode.
  // regs->CR2 &= ~ADC_CR2_CONT;

}

#if 0  
void TFTSamplesClear (uint16_t beamColour)
{
  TFT.thickness(65536*2);
  for (signalX=1 ; signalX < myHeight - 2; signalX++)
  {
    //use saved data to improve speed
    TFT.drawLine (  signalX, dataPlot[signalX-1], signalX + 1, dataPlot[signalX] , beamColour) ;
  } 
  
  TFT.foreColor565(beamColour);
  TFT.startPolyLine(myHeight-2); 
  for (signalX=0 ; signalX < myHeight - 2; signalX++)
  {
    //use saved data to improve speed
    TFT.addPolyLine(signalX + 1, dataPlot[signalX]) ;
  } 

  TFT.thickness(65536);
}
#endif 


void TFTSamples (uint16_t beamColour)
{
  //calculate first sample
#if 0
  signalY =  ((myHeight * dataPoints[0 * ((endSample - startSample) / (myWidth * timeBase / 100)) + 1]) / ANALOG_MAX_VALUE) * (yZoomFactor / 100.) + yPosition;
  dataPlot[0]=signalY * 99 / 100 + 1;
  
  for (signalX=1 ; signalX < myHeight - 2; signalX++)
  {
    // Scale our samples to fit our screen. Most scopes increase this in steps of 5,10,25,50,100 250,500,1000 etc
    // Pick the nearest suitable samples for each of our myWidth screen resolution points
    signalY1 = ((myHeight * dataPoints[(signalX + 1) * ((endSample - startSample) / (myWidth * timeBase / 100)) + 1]) / ANALOG_MAX_VALUE) * (yZoomFactor / 100) + yPosition ;
    dataPlot[signalX] = signalY1 * 99 / 100 + 1;
    TFT.drawLine (  signalX, dataPlot[signalX-1], signalX + 1, dataPlot[signalX] , beamColour) ;
    signalY = signalY1;
  }
#endif

  TFT.foreColor565(beamColour);
  TFT.startPolyLine(myHeight-2); 
  for (signalX=0 ; signalX < myHeight - 2; signalX++)
  {
    // Scale our samples to fit our screen. Most scopes increase this in steps of 5,10,25,50,100 250,500,1000 etc
    // Pick the nearest suitable samples for each of our myWidth screen resolution points
    signalY1 = (((myWidth * dataPoints[(signalX + 1) * ((endSample - startSample) / (myHeight * timeBase / 100)) + 1]) / ANALOG_MAX_VALUE) + yPosition -myWidth/2)*yZoomFactor/100.+myWidth/2;
    uint16_t y = signalY1 * 99 / 100 + 1; // //dataPlot[signalX]
    TFT.addPolyLine(signalX+1, y) ; //dataPlot[signalX]
  } 
}

/*
// Run a bunch of NOOPs to trim the inter ADC conversion gap
void sweepDelay(unsigned long sweepDelayFactor) {
  volatile unsigned long i = 0;
  for (i = 0; i < sweepDelayFactor; i++) {
    __asm__ __volatile__ ("nop");
  }
}
*/

void showLabels()
{
  //TFT.setRotation(LANDSCAPE);
  TFT.setTextSize(1);
  TFT.setCursor(10, 190);
  // TFT.print("Y=");
  //TFT.print((samplingTime * xZoomFactor) / maxSamples);
  TFT.print(float (float(samplingTime) / float(maxSamples)));

  TFT.setTextSize(1);
  TFT.print(" uS/Sample  ");
  TFT.print(maxSamples);
  TFT.print(" samples ");
//  TFT.setCursor(10, 190);
//  TFT.print(displayTime);
#if 0
  if (displayTime>0) {
    TFT.print(fps);
    TFT.print(" fps    ");
  }
#endif  
  TFT.setTextSize(2);
  TFT.setCursor(10, 210);
  TFT.print("0.3");
  TFT.setTextSize(1);
  TFT.print(" V/Div ");
  TFT.setTextSize(1);

  TFT.print("timeBase=");
  TFT.print(timeBase);
  TFT.print(" yzoom=");
  TFT.print(yZoomFactor);
  TFT.print(" ypos=");
  TFT.print(yPosition);
  //showTime();
  //TFT.setRotation(PORTRAIT);
}

void serialSamples ()
{
  // Send *all* of the samples to the serial port.
  serial_debug.println("#Time(uS), ADC Number, value, diff");
  for (int16_t j = 1; j < maxSamples   ; j++ )
  {
    // Time from trigger in milliseconds
    serial_debug.print((samplingTime / (maxSamples))*j);
    serial_debug.print(" ");
    // raw ADC data
    serial_debug.print(j % 2 + 1);
    serial_debug.print(" ");
    serial_debug.print(dataPoints[j] );
    serial_debug.print(" ");
    serial_debug.print(dataPoints[j] - dataPoints[j - 1]);
    serial_debug.print(" ");
    serial_debug.print(dataPoints[j] - ((dataPoints[j] - dataPoints[j - 1]) / 2));
    serial_debug.print("\n");

    // delay(100);


  }
  serial_debug.print("\n");
}

void toggleHold()
{
  if (triggerHeld)
  {
    triggerHeld = false;
    serial_debug.println("# Toggle Hold off");
  }
  else
  {
    triggerHeld = true;
    serial_debug.println("# Toggle Hold on");
  }
}

void toggleSerial() {
  serialOutput = !serialOutput ;
  serial_debug.println("# Toggle Serial");
  serialSamples();
}

void unrecognized(const char *command) {
  serial_debug.print("# Unknown Command.[");
  serial_debug.print(command);
  serial_debug.println("]");
}

void decreaseTimebase() {
  clearTrace();
  /*
  sweepDelayFactor =  sweepDelayFactor / 2 ;
  if (sweepDelayFactor < 1 ) {

    serial_debug.print("Timebase=");
    sweepDelayFactor = 1;
  }
  */
  if (timeBase > 100)
  {
    timeBase -= 100;
  }
  showTrace();
  serial_debug.print("# Timebase=");
  serial_debug.println(timeBase);

}

void increaseTimebase() {
  clearTrace();
  serial_debug.print("# Timebase=");
  if (timeBase < 10000)
  {
    timeBase += 100;
  }
  //sweepDelayFactor = 2 * sweepDelayFactor ;
  showTrace();
  serial_debug.print("# Timebase=");
  serial_debug.println(timeBase);
}

void increaseZoomFactor() {
  clearTrace();
  if ( yZoomFactor < 800) {
    yZoomFactor *= 2;
  }
  showTrace();

}

void decreaseZoomFactor() {
  clearTrace();
  if (yZoomFactor > 25) {
    yZoomFactor /= 2;
  }
  showTrace();
  //clearTFT();
}

void clearTrace() {
  clearTFT();
#if 0  
//  TFT.thickness(2*65536);
  TFTSamples(BEAM_OFF_COLOUR);
//  TFT.thickness(65536);
#endif
  showGraticule();
  
}

void showTrace() {
  showLabels();
  TFTSamples(BEAM1_COLOUR);
}

void scrollRight() {
  clearTrace();
  if (startSample < (endSample - 120)) {
    startSample += 100;
  }
  showTrace();
  Serial.print("# startSample=");
  Serial.println(startSample);


}

void scrollLeft() {
  clearTrace();
  if (startSample > (120)) {
    startSample -= 100;
    showTrace();
  }
  Serial.print("# startSample=");
  Serial.println(startSample);

}

void increaseYposition() {

  if (yPosition < myHeight ) {
    clearTrace();
    yPosition ++;
    showTrace();
  }
  Serial.print("# yPosition=");
  Serial.println(yPosition);
}

void decreaseYposition() {

  if (yPosition > -myHeight ) {
    clearTrace();
    yPosition --;
    showTrace();
  }
  Serial.print("# yPosition=");
  Serial.println(yPosition);
}


void increaseTriggerPosition() {

  if (triggerValue < ANALOG_MAX_VALUE ) {
    triggerValue += TRIGGER_POSITION_STEP;  //trigger position step
  }
  Serial.print("# TriggerPosition=");
  Serial.println(triggerValue);
}

void decreaseTriggerPosition() {

  if (triggerValue > 0 ) {
    triggerValue -= TRIGGER_POSITION_STEP;  //trigger position step
  }
  Serial.print("# TriggerPosition=");
  Serial.println(triggerValue);
}

void atAt() {
  serial_debug.println("# Hello");
}

void toggleTestPulseOn () {
  pinMode(TEST_WAVE_PIN, OUTPUT);
  analogWrite(TEST_WAVE_PIN, 75);
  serial_debug.println("# Test Pulse On.");
}

void toggleTestPulseOff () {
  pinMode(TEST_WAVE_PIN, INPUT);
  serial_debug.println("# Test Pulse Off.");
}

uint16 timer_set_period(HardwareTimer timer, uint32 microseconds) {
  if (!microseconds) {
    timer.setPrescaleFactor(1);
    timer.setOverflow(1);
    return timer.getOverflow();
  }

  uint32 cycles = microseconds * (72000000 / 1000000); // 72 cycles per microsecond

  uint16 ps = (uint16)((cycles >> 16) + 1);
  timer.setPrescaleFactor(ps);
  timer.setOverflow((cycles / ps) - 1 );
  return timer.getOverflow();
}

/**
* @brief Enable DMA requests
* @param dev ADC device on which to enable DMA requests
*/

void adc_dma_enable(const adc_dev * dev) {
  bb_peri_set_bit(&dev->regs->CR2, ADC_CR2_DMA_BIT, 1);
}


/**
* @brief Disable DMA requests
* @param dev ADC device on which to disable DMA requests
*/

void adc_dma_disable(const adc_dev * dev) {
  bb_peri_set_bit(&dev->regs->CR2, ADC_CR2_DMA_BIT, 0);
}

static void DMA1_CH1_Event() {
  dma1_ch1_Active = 0;
}

#ifdef SUPPORT_TIME
void setCurrentTime() {
  char *arg;
  arg = sCmd.next();
  String thisArg = arg;
  serial_debug.print("# Time command [");
  serial_debug.print(thisArg.toInt() );
  serial_debug.println("]");
  setTime(thisArg.toInt());
  time_t tt = now();
  rt.setTime(tt);
  serialCurrentTime();
}

void serialCurrentTime() {
  serial_debug.print("# Current time - ");
  if (hour(tt) < 10) {
    serial_debug.print("0");
  }
  serial_debug.print(hour(tt));
  serial_debug.print(":");
  if (minute(tt) < 10) {
    serial_debug.print("0");
  }
  serial_debug.print(minute(tt));
  serial_debug.print(":");
  if (second(tt) < 10) {
    serial_debug.print("0");
  }
  serial_debug.print(second(tt));
  serial_debug.print(" ");
  serial_debug.print(day(tt));
  serial_debug.print("/");
  serial_debug.print(month(tt));
  serial_debug.print("/");
  serial_debug.print(year(tt));
  serial_debug.println("(\"TZ\")");

}
#endif

#if defined TOUCH_SCREEN_AVAILABLE
void touchCalibrate() {
  // showGraticule();
  for (uint8_t screenLayout = 0 ; screenLayout < 4 ; screenLayout += 1)
  {
    //TFT.setRotation(screenLayout);
    TFT.setCursor(0, 10);
    TFT.print("  Press and hold centre circle ");
    TFT.setCursor(0, 20);
    TFT.print("   to calibrate touch panel.");
  }
  //TFT.setRotation(PORTRAIT);
  TFT.drawCircle(myHeight / 2, myWidth / 2, 20, GRATICULE_COLOUR);
  TFT.fillCircle(myHeight / 2, myWidth / 2, 10, BEAM1_COLOUR);
  //delay(5000);
  readTouchCalibrationCoordinates();
  clearTFT();
}

void readTouchCalibrationCoordinates()
{
  int calibrationTries = 6000;
  int failCount = 0;
  int thisCount = 0;
  uint32_t tx = 0;
  uint32_t ty = 0;
  boolean OK = false;

  while (OK == false)
  {
    while ((myTouch.dataAvailable() == false) && thisCount < calibrationTries) {
      thisCount += 1;
      delay(1);
    }
    if ((myTouch.dataAvailable() == false)) {
      return;
    }
    // myGLCD.print("*  HOLD!  *", CENTER, text_y_center);
    thisCount = 0;
    while ((myTouch.dataAvailable() == true) && (thisCount < calibrationTries) && (failCount < 10000))
    {
      myTouch.calibrateRead();
      if (!((myTouch.TP_X == 65535) || (myTouch.TP_Y == 65535)))
      {
        tx += myTouch.TP_X;
        ty += myTouch.TP_Y;
        thisCount++;
      }
      else
        failCount++;
    }
    if (thisCount >= calibrationTries)
    {
      for (thisCount = 10 ; thisCount < 100 ; thisCount += 10)
      {
        TFT.drawCircle(myHeight / 2, myWidth / 2, thisCount, GRATICULE_COLOUR);
      }
      delay(500);
      OK = true;
    }
    else
    {
      tx = 0;
      ty = 0;
      thisCount = 0;
    }
    if (failCount >= 10000)
      // Didn't calibrate so just leave calibration as is.
      return;
  }
  serial_debug.print("# Calib x: ");
  serial_debug.println(tx / thisCount, HEX);
  serial_debug.print("# Calib y: ");
  serial_debug.println(ty / thisCount, HEX);
  // Change calibration data from here..
  // cx = tx / iter;
  // cy = ty / iter;
}

void readTouch() {

  if (myTouch.dataAvailable())
  {
    myTouch.read();
    // Note: This is corrected to account for different orientation of screen origin (x=0,y=0) in Adafruit lib from UTouch lib
    uint32_t touchY = myWidth - myTouch.getX();
    uint32_t touchX = myTouch.getY();
    //

    serial_debug.print("# Touched ");
    serial_debug.print(touchX);
    serial_debug.print(",");
    serial_debug.println(touchY);

    TFT.drawPixel(touchX, touchY, BEAM2_COLOUR);
  }
}

#endif

void showCredits() {
  TFT.setTextSize(2);                           // Small 26 char / line
  //TFT.setTextColor(CURSOR_COLOUR, BEAM_OFF_COLOUR) ;
  TFT.setCursor(0, 50);
  TFT.print(" STM-O-Scope by Andy Hull") ;
  TFT.setCursor(0, 70);
  TFT.print("      Inspired by");
  TFT.setCursor(0, 90);
  TFT.print("      Ray Burnette.");
  TFT.setCursor(0, 130);
  TFT.print("      Victor PV");
  TFT.setCursor(0, 150);
  TFT.print("      Roger Clark");
  TFT.setCursor(0, 170);
  TFT.print(" and all at stm32duino.com");
  TFT.setCursor(0, 190);
  TFT.print(" CH1 Probe STM32F Pin [");
  TFT.print(analogInPin);
  TFT.print("]");
  TFT.setCursor(0, 220);
  TFT.setTextSize(1);
  TFT.print("     GNU GENERAL PUBLIC LICENSE Version 2 ");
  TFT.setTextSize(2);
  //TFT.setRotation(PORTRAIT);
}

#ifdef SUPPORT_TIME
static inline int readBKP(int registerNumber)
{
  if (registerNumber > 9)
  {
    registerNumber += 5; // skip over BKP_RTCCR,BKP_CR,BKP_CSR and 2 x Reserved registers
  }
  return *(BKP_REG_BASE + registerNumber) & 0xffff;
}

static inline void writeBKP(int registerNumber, int value)
{
  if (registerNumber > 9)
  {
    registerNumber += 5; // skip over BKP_RTCCR,BKP_CR,BKP_CSR and 2 x Reserved registers
  }

  *(BKP_REG_BASE + registerNumber) = value & 0xffff;
}

void sleepMode()
{
  serial_debug.println("# Nighty night!");
  // Set PDDS and LPDS bits for standby mode, and set Clear WUF flag (required per datasheet):
  PWR_BASE->CR |= PWR_CR_CWUF;
  PWR_BASE->CR |= PWR_CR_PDDS;

  // set sleepdeep in the system control register
  SCB_BASE->SCR |= SCB_SCR_SLEEPDEEP;

  // Now go into stop mode, wake up on interrupt
  // disableClocks();
  asm("wfi");
}
#endif
