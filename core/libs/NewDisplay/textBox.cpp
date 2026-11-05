
#include "FT812.h"

#include "display.h"
#include "textBoxControl.h"

#include "log.h"
#include <string.h>

cTextBox::cTextBox(strControlProperties* properties)
  : cDisplayControl(controlIdTextBox, 1)
{
    colorsCount = 3;
    colors = display.getTheme().defaultTextColors;

    if (properties == nullptr)
    {
        posX = 0;
        posY = 0;
        text = nullptr;
        data = nullptr;
        width = 0;
        height = 0;
        style = 0;
        value = 0;
        return;
    }

    colors = (properties->colors == nullptr) ? colors : properties->colors;

    posX = properties->x;
    posY = properties->y;

    text = properties->text;

    value = properties->value;

    data = (strTextBoxData*)(properties->data);

    width = properties->w;
    height = properties->h;

    setStyle(properties->style);
}

cTextBox::~cTextBox()
{
}

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
void cTextBox::setStyle(uint32_t style)
{
    this->style = style;

    textStyle = (style & controlStyleCenterX ? OPT_CENTERX : 0) |
                (style & controlStyleCenterY ? OPT_CENTERY : 0) |
                (style & controlStyleRightX ? OPT_RIGHTX : 0);

    int8_t textFont = FONT_INDEX_FROM_STYLE;
    textFont = (textFont >= 0) ? textFont : 0;
    font = &fonts[textFont];
}

void cTextBox::setText(const char* text)
{
    this->text = text;
}

void cTextBox::setValue(int value)
{
    this->value = value;
}

void cTextBox::setColors(const uint32_t* colors)
{
    this->colors = colors;
}

void cTextBox::setData(void* data)
{
    this->data = (strTextBoxData*)(data);
}

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------
void cTextBox::update()
{
    int16_t border_x = posX;
    int16_t border_y = posY;
    int16_t lines_step = font->height + value;
    uint8_t lines_max = (height / lines_step) - 1;
    int16_t lines_space = font->height;

    int16_t text_y;

    if (style & controlStyleCenterX)
    {
        border_x = posX - (width / 2);
    }
    if (style & controlStyleCenterY)
    {
        border_y = posY - (height / 2);
        text_y = text_y + lines_space + font->height / 2;
    }

    if (style & controlStyleBackground)
    {
        if (style & controlStyleNoTransparency)
        {
            API_BLEND_FUNC(SRC_ALPHA, ZERO);
        }

        API_COLOR(colors[1]);

        if (style & controlStyleRoundedBorder)
            API_LINE_WIDTH(32);
        else
            API_LINE_WIDTH(16);

        API_BEGIN(RECTS);
        API_VERTEX2F(border_x, border_y);
        API_VERTEX2F(border_x + font->width, border_y + font->height);
        API_END();

        if (style & controlStyleBottomShadow)
        {
            API_BLEND_FUNC(SRC_ALPHA, ZERO);

            API_SAVE_CONTEXT();
            uint16_t grad_y = posY + height - 10;
            uint16_t grad_h = 10;
            API_SCISSOR_XY(border_x, grad_y);
            API_SCISSOR_SIZE(width + 1, grad_h);
            API_CMD_GRADIENT(0, grad_y, colors[1], 0, grad_y + grad_h, 0x0);
            API_RESTORE_CONTEXT();

            API_BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        }

        if (style & controlStyleNoTransparency)
        {
            API_BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        }
    }
    if (style & controlStyleBorder)
    {
        API_COLOR(colors[2]);

        API_LINE_WIDTH(24);

        API_BEGIN(LINE_STRIP);
        API_VERTEX2F(border_x, border_y);
        API_VERTEX2F(border_x + width, border_y);
        API_VERTEX2F(border_x + width, border_y + height);
        API_VERTEX2F(border_x, border_y + height);
        API_VERTEX2F(border_x, border_y);
        API_END();
    }

    textLength = strlen(text);
    position = text;

    API_COLOR(colors[0]);

    if (text != nullptr && text[0] != 0 && textLength > 0)
    {

        for (uint8_t line = 0; line < lines_max; line++)
        {
            int16_t length = getNextLineLength();
            if (length > 0)
                API_CMD_TEXT(posX + 5,
                             posY + 5 + (line * lines_step),
                             font->handle,
                             textStyle,
                             position,
                             length);
            else if (length < 0)
                break;
            // else if 0 = pusta linia

            position += length + 1;

            if ((uint32_t)(position - text) > textLength)
                break;
        }
    }
}

int16_t cTextBox::getNextLineLength()
{
    // width; // niby powinno patrzec na szerokosc miejsca i jesli dlugi wyraz nie wyjsc poza i ciąc
    // wyraz wtedy ale...

    const char* temp_pos = position;
    int16_t last_length = 0;

    uint16_t enterPositionIdx = 0;
    uint16_t spacePositionIdx = 0;

    uint16_t maxTextWidth = width - 5;

    constexpr uint8_t MAX_WHITE_CHARTACTERS_IN_LINE = 30;

    for (uint8_t whiteCharCounter = 0; whiteCharCounter < MAX_WHITE_CHARTACTERS_IN_LINE;
         whiteCharCounter++)
    {

        const char* enter_pos = strchr(temp_pos, '\n');
        bool enterNotFound = (enter_pos == nullptr);
        enterPositionIdx =
          last_length + (enterNotFound ? (strlen(position)) : (uint16_t)(enter_pos - temp_pos));

        if (!enterPositionIdx)
            return 0;

        const char* space_pos = strchr(temp_pos, ' ');
        bool spaceNotFound = (space_pos == nullptr);
        spacePositionIdx =
          last_length + (spaceNotFound ? (strlen(position)) : (uint16_t)(space_pos - temp_pos));

        if (enterPositionIdx <= spacePositionIdx)
        {
            if ((getTextWidth((strFont*)font, position, enterPositionIdx) > maxTextWidth) &&
                (whiteCharCounter > 0))
            {

                return last_length - 1;
            }
            else
            {
                return enterPositionIdx;
            }
        }

        bool isNotEnoughSpaceInLine =
          getTextWidth((strFont*)font, position, spacePositionIdx) > maxTextWidth;

        if (isNotEnoughSpaceInLine)
        {
            if (whiteCharCounter > 0)
                return last_length - 1; // wraca na pozycje bialego znaku
            else
                return spacePositionIdx;
        }

        last_length = spacePositionIdx + 1; // omija bialy znak dla dalszego processingu
        temp_pos = space_pos + 1;
    }

    return -1;
}
