#include "mg513_gmr500ppr.h"
#include "stm32f407xx.h"
#include "stm32f4xx_hal_tim.h"
#include "tim.h"


MG513_GMR500PPR mg513_gmr500ppr_motor[4];
int maxccr[4];


#define LimitMax(input, max)   \
    {                          \
        if (input > max)       \
        {                      \
            input = max;       \
        }                      \
        else if (input < -max) \
        {                      \
            input = -max;      \
        }                      \
    }


extern "C"
void StartDefaultTask(void const * argument)
{
    //初始化编码器
    mg513_gmr500ppr_motor[0].encoder.Init(&motor0_encoder_htim);

    //初始化电机驱动器
    mg513_gmr500ppr_motor[0].at8236_cmd.Init(&motor0_pwma_htim,MOTOR0_PWMA_TIM_Channel,&motor0_pwmb_htim,MOTOR0_PWMB_TIM_Channel,500);

    //定时器中断
    HAL_TIM_Base_Start_IT(&htim6);

    for(;;)
    {
        mg513_gmr500ppr_motor[0].at8236_cmd.PWM_Pulse_CMD(0);
        osDelay(1);
    }
}



extern "C"
/**
 * @brief       定时器更新中断回调函数
 * @param        htim:定时器句柄指针
 * @note        此函数会被定时器中断函数共同调用的
 * @retval      无
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    /* FreeRTOS的时基，需要注释掉main.c中的HAL_TIM_PeriodElapsedCallback函数 */
    if (htim->Instance == TIM7) 
    {
    HAL_IncTick();
    }

    /* 电机编码器 */
	if(htim->Instance == MOTOR0_ENCODER_TIM_BASE)
	{
		if(__HAL_TIM_IS_TIM_COUNTING_DOWN(&motor0_encoder_htim) == 1)                   /* 判断CR1的DIR位 */
		{
			mg513_gmr500ppr_motor[0].encoder.motor_encoder_count--;                           /* DIR位为1，也就是递减计数 */
		}
		else
		{
			mg513_gmr500ppr_motor[0].encoder.motor_encoder_count++;                          /* DIR位为0，也就是递增计数 */
		}
	}

    /* 电机编码器运行中断 */
	if(htim->Instance == TIM6)
	{
	    mg513_gmr500ppr_motor[0].encoder.get_finall_encoder_value(&motor0_encoder_htim);
	}
}




void MG513_GMR500PPR::AT8236_Cmd::Init(TIM_HandleTypeDef *htim_a,uint32_t TIM_Channel_a,TIM_HandleTypeDef *htim_b,uint32_t TIM_Channel_b,int maxpulse)
{
    this->htim_pwma = htim_a;
    this->TIM_Channel_Pwma = TIM_Channel_a;
    this->htim_pwmb = htim_b;
    this->TIM_Channel_Pwmb = TIM_Channel_b;
    this->max_pulse = maxpulse;

    HAL_TIM_PWM_Start(htim_a,TIM_Channel_a);
    HAL_TIM_PWM_Start(htim_b,TIM_Channel_b);

}


/**
 * @brief       AT8236电机驱动控制
 * @param       无
 * @retval      编码器值
 */
void MG513_GMR500PPR::AT8236_Cmd::PWM_Pulse_CMD(int pulse)
{
    LimitMax(pulse,this->max_pulse);

    if(pulse > 0)   //正转
    {
        __HAL_TIM_SetCompare(this->htim_pwma,this->TIM_Channel_Pwma,pulse);
        __HAL_TIM_SetCompare(this->htim_pwmb,this->TIM_Channel_Pwmb,0);
    }
    else if(pulse < 0)   //反转
    {
        __HAL_TIM_SetCompare(this->htim_pwma,this->TIM_Channel_Pwma,0);
        __HAL_TIM_SetCompare(this->htim_pwmb,this->TIM_Channel_Pwmb,pulse);
    }
    else  //制动
    {
        __HAL_TIM_SetCompare(this->htim_pwma,this->TIM_Channel_Pwma,0);
        __HAL_TIM_SetCompare(this->htim_pwmb,this->TIM_Channel_Pwmb,0);
    }
    
}


void MG513_GMR500PPR::Encoder::Init(TIM_HandleTypeDef *htim)
{
    this->htim_encoder = htim;

    HAL_TIM_Encoder_Start_IT(htim_encoder,TIM_CHANNEL_1);
	HAL_TIM_Encoder_Start_IT(htim_encoder,TIM_CHANNEL_2);

}


void MG513_GMR500PPR::Encoder::get_finall_encoder_value(TIM_HandleTypeDef *htim)
{
    encoder_data.encoder_now = get_encoder_value(htim);                             /* 获取编码器值，用于计算速度 */
    motor_message_filtering(&encoder_data,&motor_data,LPF_Q,GAP_TIME_MS);      /* 中位平均值滤除编码器抖动数据，50ms计算一次速度*/
}


/**
 * @brief       获取编码器的值
 * @param       无
 * @retval      编码器值
 */
int32_t MG513_GMR500PPR::Encoder::get_encoder_value(TIM_HandleTypeDef *htim)
{
	int32_t encoder_value;
	encoder_value = __HAL_TIM_GET_COUNTER(htim) + motor_encoder_count * 65536;       /* 当前计数值+之前累计编码器的值=总的编码器值 */
	
	return encoder_value;
}


/**
 * @brief       电机速度计算
 * @param       encode_now：当前编码器总的计数值
 *              ms：计算速度的间隔，中断1ms进入一次，例如ms = 5即5ms计算一次速度
 *              q: 低通滤波系数
 * @retval      无
 * @attention   
 */
void MG513_GMR500PPR::Encoder::motor_message_filtering(Encoder_TypeDef *encoder,Motor_TypeDef *motor,fp32 q,uint8_t ms)
{
	   //冒泡排序法
    uint8_t i = 0, j = 0;
		static uint8_t bubble_sort_number = 0;
		//缓存，充当冒泡排序法的交换缓存和冒泡排序法处理后的速度总和缓存
    fp32 temp = 0.0f;
	
    static uint8_t time_count = 0;
    static fp32 speed_arr[10] = {0.0f};                     /* 存储速度进行滤波运算 */

    if (time_count == ms)                                     /* 计算一次速度 */
    {
        /* 计算电机转速 
           第一步 ：计算ms毫秒内计数变化量
           第二步 ；计算1min内计数变化量：g_encode.speed * ((1000 / ms) * 60 ，
           第三步 ：除以编码器旋转一圈的计数次数（倍频倍数 * 编码器分辨率）
           第四步 ：除以减速比即可得出电机转速
        */
			
        encoder->encoder_delta = (encoder->encoder_now - encoder->encoder_pre);    /* 计算编码器计数值的变化量 */
			if(ABS(encoder->encoder_delta) >= 60000.0f)
			{
				encoder->encoder_delta = 0.0f;
			}
        encoder->encoder_delta_sum += encoder->encoder_delta;
        speed_arr[bubble_sort_number++] = (fp32)(encoder->encoder_delta * ((1000 / ms) * 60.0) / (FRE_DOU_RATIO) / (PULSE_PER_REVOLUTION));    /* 保存电机转速 */
        
        encoder->encoder_pre = encoder->encoder_now;          /* 保存当前编码器的值 */

        /* 累计10次速度值，后续进行滤波*/
        if (bubble_sort_number == 10)
        {
            for (i = 10; i >= 1; i--)                       /* 冒泡排序*/
            {
                for (j = 0; j < (i - 1); j++) 
                {
                    if (speed_arr[j] > speed_arr[j + 1])    /* 数值比较 */
                    { 
                        temp = speed_arr[j];                /* 数值换位 */
                        speed_arr[j] = speed_arr[j + 1];
                        speed_arr[j + 1] = temp;
                    }
                }
            }
						
            temp = 0.0;
            
            for (i = 2; i < 8; i++)                         /* 去除两边高低数据 */
            {
                temp += speed_arr[i];                       /* 将中间数值累加 */
            }
            
            temp = (fp32)(temp / 6);                       /*求速度平均值*/
            
            /* 一阶低通滤波
             * 公式为：Y(n)= qX(n) + (1-q)Y(n-1)
             * 其中X(n)为本次采样值；Y(n-1)为上次滤波输出值；Y(n)为本次滤波输出值，q为滤波系数
             * q值越小则上一次输出对本次输出影响越大，整体曲线越平稳，但是对于速度变化的响应也会越慢
             */
            motor->rotor_speed = (fp32)((fp32)(q * temp) + (motor->rotor_speed * (fp32)(1.0f-q)) );  //通过低通滤波计算转子速度
						motor->motor_speed = motor->rotor_speed / (REDUCTION_RATIO);     //计算减速后电机转轴的速度
						
						motor->rotor_position =  encoder->encoder_delta_sum;//电机位置
						motor->motor_position =  motor->rotor_position / (REDUCTION_RATIO);  //计算减速后电机转轴的位置
						
						motor->rotor_round_cnt = motor->rotor_position / (FRE_DOU_RATIO) / (PULSE_PER_REVOLUTION);
						motor->motor_round_cnt = motor->rotor_round_cnt / (REDUCTION_RATIO);  //计算减速后电机转轴的圈数
							
            bubble_sort_number = 0;
        }
        time_count = 0;
    }
    time_count ++;
}
