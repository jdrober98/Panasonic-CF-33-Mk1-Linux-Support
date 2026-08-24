#include <linux/clk.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/kernel.h>
#include "mfd/tps68470.h"

#define work_to_led(work) \
    container_of(work, struct tps68470_led, keepalive_work)

#define lcdev_to_led(led_cdev) \
    container_of(led_cdev, struct tps68470_led, lcdev)

#define led_to_tps68470(led, index) \
	container_of(led, struct tps68470_device, leds[index])


enum tps68470_led_ids {
    TPS68470_ILED_A,
    TPS68470_ILED_B,
    TPS68470_WLED,
    TPS68470_NUM_LEDS
};

static const char *tps68470_led_names[] = {
    [TPS68470_ILED_A] = "tps68470-iled_a",
 	[TPS68470_ILED_B] = "tps68470-iled_b",
    [TPS68470_WLED] = "tps68470-wled",
};

struct tps68470_led {
    unsigned int led_id;
    struct led_classdev lcdev;
    enum led_brightness state;
    struct work_struct keepalive_work;
};

struct tps68470_device {
    struct clk *clk;
    struct device *dev;
    struct regmap *regmap;
    struct tps68470_led leds[TPS68470_NUM_LEDS];
};

static int tps68470_leds_init(struct tps68470_device *tps68470)
{
	int ret;

    //Max Current Values

	ret = regmap_write(
        tps68470->regmap,
        TPS68470_REG_WLEDMAXT,
		0x1 & TPS68470_WLEDMAXT_MAX_CUR_MASK
    );
	if (ret)
		return dev_err_probe(tps68470->dev, ret, "failed to set WLEDMAXT\n");

    ret = regmap_write(
        tps68470->regmap,
        TPS68470_REG_WLEDC1,
		0x1 & TPS68470_WLEDC_ILED_MASK
    );
	if (ret)
		return dev_err_probe(tps68470->dev, ret, "failed to set WLEDC1\n");

    //Initial Config
    ret = regmap_write(
        tps68470->regmap,
        TPS68470_REG_WLEDCTL,
		0x1 & TPS68470_WLED_MODE_MASK
    );
	if (ret)
		return dev_err_probe(tps68470->dev, ret, "failed to set WLEDCTL\n");

    ret = regmap_update_bits(tps68470->regmap, TPS68470_REG_WLEDCTL,
				 TPS68470_WLED_DISLED2,
				 true ? TPS68470_WLED_DISLED2 : 0);
	if (ret)
		dev_err_probe(tps68470->dev, ret, "failed to set DISLED2\n");

    return 0;
}

/*
 * The WLED can operate in different modes, including a Flash and Torch mode. In
 * each mode there's a timeout which ranges from a matter of milliseconds to up
 * to 13 seconds. We don't want that timeout to apply though because the LED
 * should be lit until we say that it should no longer be lit, re-trigger the
 * LED periodically to keep it alive.
 */
static void tps68470_wled_keepalive_work(struct work_struct *work)
{
	struct tps68470_device *tps68470;
	struct tps68470_led *led;

	led = work_to_led(work);
	tps68470 = led_to_tps68470(led, led->led_id);

	regmap_write_bits(tps68470->regmap, TPS68470_REG_WLEDCTL,
				TPS68470_WLED_MODE_MASK, 0x1);

	schedule_work(&led->keepalive_work);
}

static int tps68470_brightness_set(struct led_classdev *led_cdev, enum led_brightness brightness)
{
    struct tps68470_led *led = lcdev_to_led(led_cdev);
 	struct tps68470_device *tps68470 = led_to_tps68470(led, led->led_id);
 	struct regmap *regmap = tps68470->regmap;
    int ret;

    switch (led->led_id) {
        case TPS68470_ILED_A:
            ret = regmap_update_bits(
                regmap,
                TPS68470_REG_ILEDCTL,
                TPS68470_ILEDCTL_ENA,
                brightness ? TPS68470_ILEDCTL_ENA : 0
            );
            break;
        case TPS68470_ILED_B:
            //Likely wouldn't matter but no point doing anything with nothing attached.

            // ret = regmap_update_bits(
            //     regmap,
            //     TPS68470_REG_ILEDCTL,
            //     TPS68470_ILEDCTL_ENB,
            //     brightness ? TPS68470_ILEDCTL_ENB : 0
            // );
            break;
        case TPS68470_WLED:
            /*
            * LED core does not prevent re-setting brightness to its current
            * value; we need to do so here to avoid unbalanced calls to clk
            * enable/disable.
            */
            if (led->state == brightness)
                return 0;

            if (brightness) {

                schedule_work(&led->keepalive_work);

                ret = clk_prepare_enable(tps68470->clk);
			    if (ret)
				    goto err_cancel_work;
            } else {
                cancel_work_sync(&led->keepalive_work);
                clk_disable_unprepare(tps68470->clk);
            }
                
            ret = regmap_update_bits(tps68470->regmap, TPS68470_REG_WLEDCTL,
                    TPS68470_WLED_EN_MASK,
                    brightness ? TPS68470_WLED_EN_MASK :
                            ~TPS68470_WLED_EN_MASK);
            if (ret)
                goto err_disable_clk;

            ret = regmap_update_bits(
                tps68470->regmap,
                TPS68470_REG_WLEDCTL,
                TPS68470_WLED_CTL_MASK,
                brightness ? TPS68470_WLED_CTL_MASK : ~TPS68470_WLED_CTL_MASK);
            if (ret) 
                goto err_disable_clk;

            led->state = brightness;

            break;
        default:
            pr_info("LED ID: %d", led->led_id);
            return dev_err_probe(led_cdev->dev, -EINVAL, "invalid LED ID\n");
    }

    return ret;

    err_disable_clk:
	    clk_disable_unprepare(tps68470->clk);
    err_cancel_work:
	    cancel_work_sync(&led->keepalive_work);
	    return dev_err_probe(tps68470->dev, ret, "Error\n");
}

static enum led_brightness tps68470_brightness_get(struct led_classdev *led_cdev)
{
    struct tps68470_led *led = lcdev_to_led(led_cdev);
 	struct tps68470_device *tps68470 = led_to_tps68470(led, led->led_id);
 	struct regmap *regmap = tps68470->regmap;
    int ret;
    int value = 0;

    ret = regmap_read(regmap, TPS68470_REG_ILEDCTL, &value);
    if (ret)
        return dev_err_probe(led_cdev->dev, -EINVAL, "failed on reading ILED register\n");

    switch (led->led_id) {
        case TPS68470_ILED_A:
            value = value & TPS68470_ILEDCTL_ENA;
            break;
        case TPS68470_ILED_B:
            value = value & TPS68470_ILEDCTL_ENB;
            break;
        case TPS68470_WLED:
            ret = regmap_read(regmap, TPS68470_REG_WLEDCTL, &value);
            if (ret)
			return dev_err_probe(led_cdev->dev, ret, "failed to read WLED register\n");
            value &= TPS68470_WLED_CTL_MASK;
            break;
        default:
            return dev_err_probe(led_cdev->dev, -EINVAL, "invalid LED ID\n");
    }

    return value ? LED_ON : LED_OFF;
}

static int tps68470_leds_probe(struct platform_device *pdev)
{
    int ret = 0;
    int i = 0;
    struct tps68470_device *tps68470;
    struct tps68470_led *led;
    struct led_classdev *lcdev;

    tps68470 = devm_kzalloc(&pdev->dev, sizeof(struct tps68470_device), GFP_KERNEL);
    
    if (!tps68470)
        return -ENOMEM;

    tps68470->dev = &pdev->dev;
    tps68470->regmap = dev_get_drvdata(pdev->dev.parent);

    tps68470->clk = devm_clk_get(tps68470->dev, NULL);
	if (IS_ERR(tps68470->clk))
		return dev_err_probe(tps68470->dev, PTR_ERR(tps68470->clk), "failed to get clock\n");

    for (i = 0; i < TPS68470_NUM_LEDS; i++) {
        led = &tps68470->leds[i];
        lcdev = &led->lcdev;

        led->led_id = i;

        lcdev->name = devm_kasprintf(
            tps68470->dev,
            GFP_KERNEL,
            "%s::%s",
            tps68470_led_names[i],
            LED_FUNCTION_INDICATOR
        );

        if (!lcdev->name)
            return -ENOMEM;

        lcdev->max_brightness = 1;
        lcdev->brightness = 0;
        lcdev->brightness_set_blocking = tps68470_brightness_set;
        lcdev->brightness_get = tps68470_brightness_get;
        lcdev->dev = &pdev->dev;

        ret = devm_led_classdev_register(tps68470->dev, lcdev);
        if (ret) {
            dev_err_probe(tps68470->dev, ret, "error registering led\n");
            goto err_exit;
        }

        if (led->led_id == TPS68470_WLED) {
			INIT_WORK(&led->keepalive_work,
				  tps68470_wled_keepalive_work);
		}
    }

    ret = tps68470_leds_init(tps68470);

    if (ret)
        goto err_exit;

    err_exit:
        if (ret) {
            for (i = 0; i < TPS68470_NUM_LEDS; i++) {
                if (tps68470->leds[i].lcdev.name)
                    devm_led_classdev_unregister(&pdev->dev, &tps68470->leds[i].lcdev);
            }
        }

        return ret;
}

static struct platform_driver tps68470_led_driver = {
    .driver = {
        .name = "tps68470-led",
    },
    .probe = tps68470_leds_probe,
};

module_platform_driver(tps68470_led_driver);

MODULE_ALIAS("platform:tps68470-led");
MODULE_DESCRIPTION("LED driver for TPS68470 PMIC");
MODULE_LICENSE("GPL");