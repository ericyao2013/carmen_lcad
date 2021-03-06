CON
  _clkmode = xtal1 + pll16x
  _xinfreq = 5_000_000

OBJ
  LED : "LED"
  
VAR
  long stack[100]
  long cog

PUB Main
  '  orientation: the "YS" logo is the top

  LED.InsertText(string("BEOBOT "))
  LED.Rotate ( 0 ) 'Allowed value: 0 , 90 , 180 , 270
                   '  default: 0
  LED.ScrollDir ( 2 ) 'Scroll Direction: 0=right, 1=down, 2=left, 3=up
                      'Description: The way text move, up means the first letter is on the top and last letter on the bottom
                      '  The scroll that make sense is left and up.
                      '  other scroll directions would make the text reversed
                      '  default: right ( 0 )
                      
  LED.ScrollEnable   'Enable Scroll Function, default: disable
  '┌───────────────────────────────┐
  '│         8-bit Colour          │    
  '├───────────┬───────────┬───────┤
  '│    Red    │   Green   │ Blue  │
  '├───┬───┬───┼───┬───┬───┼───┬───┤
  '│ 7 │ 6 │ 5 │ 4 │ 3 │ 2 │ 1 │ 0 │
  '└───┴───┴───┴───┴───┴───┴───┴───┘
  ' Common Color:
  ' 00 Black    FF White
  ' E0 Red      FC Yellow(kinda orange-ish)
  ' 1C Green    E3 Magenta(Looks more like a pink)
  ' 03 Blue     1F Teal(Cyan/Light Blue)
  ' F0 Orange   43 Purple
  LED.SetColor ($FC,$00) 'SetColor (Foreground color, Background color)
                         ' default: fore = black, back = black

  LED.Start(8,9,10) 'mosi, cs, sclk pins  
  'TODO: cog set up
  'TODO: make the YS is the bottom as the manufacturer do     