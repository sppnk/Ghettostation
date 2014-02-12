/**
 ******************************************************************************
 *
 * @file       GhettoStation.ino
 * @author     Guillaume S
 * @brief      Arduino based antenna tracker & telemetry display for UAV projects.
 * @project	   https://code.google.com/p/ghettostation/
 * 
 *             
 *             
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/

#include "Config.h"
#include <avr/pgmspace.h>

#include <Wire.h> 



#ifdef BEARING_METHOD_4 //use additional hmc5883L mag breakout
//HMC5883L i2c mag b
#include <HMC5883L.h>
#endif

#include <LiquidCrystal_I2C.h>

#include <Metro.h>
#include <MenuSystem.h>
#include <Button.h>
#include <Servo.h>
#include <EEPROM.h>
#include <Flash.h>


#include "Eeprom.h"
#include "GhettoStation.h"



#ifdef PROTOCOL_UAVTALK
#include "UAVTalk.cpp"
#endif

#ifdef PROTOCOL_MSP
#include "MSP.cpp"
#endif

#ifdef PROTOCOL_LIGHTTELEMETRY
#include "LightTelemetry.cpp"
#endif

#ifdef PROTOCOL_MAVLINK
#include <mavlink.h>
#include "Mavlink.cpp"
#endif



//################################### SETTING OBJECTS ###############################################






// Set the pins on the I2C chip used for LCD connections:
//                    addr, en,rw,rs,d4,d5,d6,d7,bl,blpol
LiquidCrystal_I2C LCD(I2CADDRESS, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);  // LCM1602 IIC A0 A1 A2 & YwRobot Arduino LCM1602 IIC V1" 
//iquidCrystal_I2C lcd(I2CADDRESS, 4, 5, 6, 0, 1, 2, 3, 7, NEGATIVE);  // Arduino-IIC-LCD GY-LCD-V1



//##### SERVOS 

//Declaring pan/tilt servos using ServoEaser library
 Servo pan_servo;
 Servo tilt_servo;

//#####	LOOP RATES
Metro telemetryMetro = Metro(100);
Metro lcdMetro = Metro(100);
Metro buttonMetro = Metro(100);
Metro activityMetro = Metro(200);
#if defined(SIMUGPS)
Metro simugpsMetro = Metro(1000);
#endif
#if defined(DEBUG)
//Debug output
Metro debugMetro = Metro(1000); // output serial debug data each second.
#endif


//##### BUTTONS 
Button right_button = Button(RIGHT_BUTTON_PIN,BUTTON_PULLUP_INTERNAL);
Button left_button = Button(LEFT_BUTTON_PIN,BUTTON_PULLUP_INTERNAL);
Button enter_button = Button(ENTER_BUTTON_PIN,BUTTON_PULLUP_INTERNAL);

#if defined(BEARING_METHOD_4)
HMC5883L compass;
#endif

//#################################### SETUP LOOP ####################################################

void setup() {


//init LCD
init_lcdscreen();



	//init setup
	init_menu();
	
	// retrieve configuration from EEPROM
        current_bank = EEPROM.read(0);
        if (current_bank > 3) {
           current_bank = 0;
           EEPROM.write(0,0);
        }
	EEPROM_read(config_bank[int(current_bank)], configuration);
        // set temp value for servo pwm config
        servoconf_tmp[0] = configuration.pan_minpwm;
        servoconf_tmp[1] = configuration.pan_maxpwm;
        servoconf_tmp[2] = configuration.tilt_minpwm;
        servoconf_tmp[3] = configuration.tilt_maxpwm;
        delay(20);
	//clear eeprom & write default parameters if config is empty or wrong
	if (configuration.config_crc != CONFIG_VERSION) {
		clear_eeprom();
                delay(20);
		}


         //start serial com	
	init_serial();
         
	
	// attach servos 
	attach_servo(pan_servo, PAN_SERVOPIN, configuration.pan_minpwm, configuration.pan_maxpwm);
	attach_servo(tilt_servo, TILT_SERVOPIN, configuration.tilt_minpwm, configuration.tilt_maxpwm); 

        
	// move servo to neutral pan & 30° tilt at startup to prevent forcing on endpoints if misconfigured
        servoPathfinder(0, 20);
       
       // setup button callback events
       enter_button.releaseHandler(enterButtonReleaseEvents);
       left_button.releaseHandler(leftButtonReleaseEvents);
       right_button.releaseHandler(rightButtonReleaseEvents);
       
#ifdef SIMUGPS
  set_simugps();
#endif

//COMPASS
#if defined(BEARING_METHOD_4)
  compass = HMC5883L(); // Construct a new HMC5883 compass.
  delay(100);
  compass.SetScale(1.3); // Set the scale of the compass.
  compass.SetMeasurementMode(Measurement_Continuous); // Set the measurement mode to Continuous
#endif

}

//######################################## MAIN LOOP #####################################################################
void loop() {

  //update buttons internal states
  if (buttonMetro.check() == 1) {
        enter_button.isPressed();
        left_button.isPressed();
        right_button.isPressed();
  }
  
  if (current_activity==1 || current_activity==2) {    
#ifdef SIMUGPS
  simulate_gps();
#else
	//get telemetry data 
  get_telemetry();
#endif
  }
  check_activity();     
  refresh_lcd();
#if defined(DEBUG)
  debug();
#endif
        
}


//######################################## ACTIVITIES #####################################################################

void check_activity() {
    
    if (activityMetro.check() == 1) 
      {
      if (uav_satellites_visible >= 5) { gps_fix = true; } else gps_fix = false;
        
   	  if (current_activity == 0) { //MENU
		display_menu();
                servoPathfinder(0, 20); // refresh servo to prevent idle jitter
                if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press 
                   displaymenu.back();
                }


	  }
          if (current_activity == 1 ) { //TRACK
           if ((!home_pos) || (!home_bear)) {  // check if home is set before start tracking
             servoPathfinder(0, 20); // refresh servo to prevent idle jitter
             current_activity = 2; //set bearing if not set.
           } else if (home_bear) {
             antenna_tracking();
             lcddisp_tracking();
                if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press 
                   current_activity = 0;
                   telemetry_off();
                }
           }
          }
          
          if (current_activity == 2) { //SET HOME
            if (!home_pos) { lcddisp_sethome(); }
            else if (home_pos) {

              if (!home_bear) { 
              #ifndef BEARING_METHOD_3
              lcddisp_setbearing();   
              #else
              home_bearing = 0;
              #endif
            }
              else { lcddisp_homeok(); }
            }
            if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press
                   current_activity = 0;
                }
          }
          
          if (current_activity == 3) { //PAN_MINPWM
            servoconf_tmp[0] = config_servo(1, 1, servoconf_tmp[0] );
            if (servoconf_tmp[0]<configuration.pan_minpwm) {
                  attach_servo(pan_servo, PAN_SERVOPIN, servoconf_tmp[0], configuration.pan_maxpwm);
                  } 
            pan_servo.writeMicroseconds(servoconf_tmp[0]);
           
            
            if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press 
               configuration.pan_minpwm = servoconf_tmp[0];
               EEPROM_write(config_bank[int(current_bank)], configuration);
               attach_servo(pan_servo, PAN_SERVOPIN, configuration.pan_minpwm, configuration.pan_maxpwm);
               move_servo(pan_servo, 1, 0, configuration.pan_minangle, configuration.pan_maxangle);
               current_activity=0;
                }
          }
          if (current_activity == 4) { //PAN_MINANGLE
             configuration.pan_minangle = config_servo(1, 2, configuration.pan_minangle );
            pan_servo.writeMicroseconds(configuration.pan_minpwm);
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               move_servo(pan_servo, 1, 0, configuration.pan_minangle, configuration.pan_maxangle);
               current_activity=0;
                }
             
          }
          if (current_activity == 5) { //PAN_MAXPWM
             servoconf_tmp[1] = config_servo(1, 3, servoconf_tmp[1] );
            if (servoconf_tmp[1]>configuration.pan_maxpwm) {
                  attach_servo(pan_servo,PAN_SERVOPIN, configuration.pan_minpwm, servoconf_tmp[1]);
                  } 
             pan_servo.writeMicroseconds(servoconf_tmp[1]);
            
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               configuration.pan_maxpwm = servoconf_tmp[1];
               EEPROM_write(config_bank[int(current_bank)], configuration);
               attach_servo(pan_servo, PAN_SERVOPIN, configuration.pan_minpwm, configuration.pan_maxpwm);
               move_servo(pan_servo, 1, 0, configuration.pan_minangle, configuration.pan_maxangle);
               current_activity=0;
                }
          }
          
          if (current_activity == 6) { //PAN_MAXANGLE
             configuration.pan_maxangle = config_servo(1, 4, configuration.pan_maxangle );
            pan_servo.writeMicroseconds(configuration.pan_maxpwm);
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               move_servo(pan_servo, 1, 0, configuration.pan_minangle, configuration.pan_maxangle);
               current_activity=0;
                }
          }
          
          if (current_activity == 7) { //"TILT_MINPWM"
	     servoconf_tmp[2] = config_servo(2, 1, servoconf_tmp[2] );
             if (servoconf_tmp[2]<configuration.tilt_minpwm) {
              attach_servo(tilt_servo, TILT_SERVOPIN, servoconf_tmp[2], configuration.tilt_maxpwm); 
              }
             tilt_servo.writeMicroseconds(servoconf_tmp[2]); 
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               configuration.tilt_minpwm = servoconf_tmp[2];
               EEPROM_write(config_bank[int(current_bank)], configuration);
	       attach_servo(tilt_servo,TILT_SERVOPIN, configuration.tilt_minpwm, configuration.tilt_maxpwm);
               move_servo(tilt_servo, 2, 0, configuration.tilt_minangle, configuration.tilt_maxangle);;
               current_activity=0;
                }
          }
          
          if (current_activity == 8) { //TILT_MINANGLE
             configuration.tilt_minangle = config_servo(2, 2, configuration.tilt_minangle ); 
             tilt_servo.writeMicroseconds(configuration.tilt_minpwm);
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               move_servo(tilt_servo, 2, 0, configuration.tilt_minangle, configuration.tilt_maxangle);
               current_activity=0;
                }
          }
          
          if (current_activity == 9) { //"TILT_MAXPWM"
             servoconf_tmp[3] = config_servo(2, 3, servoconf_tmp[3] );
             if (servoconf_tmp[3]>configuration.tilt_maxpwm) {
              attach_servo(tilt_servo, TILT_SERVOPIN, configuration.tilt_minpwm, servoconf_tmp[3]); 
              }
             tilt_servo.writeMicroseconds(servoconf_tmp[3]);
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               configuration.tilt_maxpwm = servoconf_tmp[3];
               EEPROM_write(config_bank[int(current_bank)], configuration);
	       attach_servo(tilt_servo,TILT_SERVOPIN, configuration.tilt_minpwm, configuration.tilt_maxpwm);
               move_servo(tilt_servo, 2, 0, configuration.tilt_minangle, configuration.tilt_maxangle);
               current_activity=0;
                }
          }
          
          if (current_activity == 10) { //TILT_MAXANGLE
             configuration.tilt_maxangle = config_servo(2, 4, configuration.tilt_maxangle );
             tilt_servo.writeMicroseconds(configuration.tilt_maxpwm);
             if (enter_button.holdTime() >= 1000 && enter_button.held()) {//long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               move_servo(tilt_servo, 2, 0, configuration.tilt_minangle, configuration.tilt_maxangle);
               current_activity=0;
                }
          }

          if (current_activity == 11) { //TEST_SERVO
             test_servos();
             current_activity = 0; 
          }
          
          if (current_activity == 12) { //Configure Telemetry
             lcddisp_telemetry();
            if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               current_activity=0;
                }
          }
          if (current_activity == 13) { //Configure Baudrate
             lcddisp_baudrate();
             if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press
               EEPROM_write(config_bank[int(current_bank)], configuration);
               current_activity=0;
             }
           }
          if (current_activity == 14) { //Change settings bank
               lcddisp_bank();
             if (enter_button.holdTime() >= 1000 && enter_button.held()) { //long press
               EEPROM.write(0,current_bank);
               EEPROM_read(config_bank[int(current_bank)], configuration);
               servoconf_tmp[0] = configuration.pan_minpwm;
               servoconf_tmp[1] = configuration.pan_maxpwm;
               servoconf_tmp[2] = configuration.tilt_minpwm;
               servoconf_tmp[3] = configuration.tilt_maxpwm;
               current_activity=0;
             }
           }
      }
}

//######################################## BUTTONS #####################################################################
// enter button
void enterButtonReleaseEvents(Button &btn)
 {
     //Serial.println(current_activity);  
     if ( enter_button.holdTime() < 1000 ) { // normal press
       
        if ( current_activity == 0 ) { //button action depends activity state
            displaymenu.select();
           }
         else if ( current_activity == 2 ) {
            if ((gps_fix) && (!home_pos)) {
              //saving home position
              home_lat = uav_lat;
              home_lon = uav_lon;
              home_alt = uav_alt;
              home_pos = true;
            }
            
            else if ((gps_fix) && (home_pos) && (!home_bear)) {
             // saving home bearing
#ifdef BEARING_METHOD_1             
                 //set_bearing(); 
                 home_bearing = calc_bearing(home_lon, home_lat, uav_lon, uav_lat); // store bearing relative to north
                 home_bear = true;
#else
                //bearing reference is set manually from a compass
                 home_bear = true;
#endif
                 configuration.bearing = home_bearing;
                 EEPROM_write(config_bank[int(current_bank)], configuration);
            }
            else if ((gps_fix) && (home_pos) && (home_bear)) {
              // START TRACKING 
              current_activity = 1;

            }
        }
        
     }
     
 }


// left button
void leftButtonReleaseEvents(Button &btn)
{
  if ( left_button.holdTime() < 1000 ) {

    if (current_activity==0) {
        displaymenu.prev();
    }
    
    else if ( current_activity != 0 && current_activity != 1 && current_activity != 2 ) {
              //We're in a setting area: Left button decrase current value.
          if (current_activity == 3) servoconf_tmp[0]--;		 
          if (current_activity == 4) configuration.pan_minangle--;
          if (current_activity == 5) servoconf_tmp[1]--;
          if (current_activity == 6) configuration.pan_maxangle--;
          if (current_activity == 7) servoconf_tmp[2]--;
          if (current_activity == 8) configuration.tilt_minangle--;        
          if (current_activity == 9) servoconf_tmp[3]--;
          if (current_activity == 10) configuration.tilt_maxangle--;
          if (current_activity == 12) {
             if (configuration.telemetry > 0) {
               configuration.telemetry -= 1;
             }       
          }
          if (current_activity == 13) {
             if (configuration.baudrate > 0) {
               configuration.baudrate -= 1;
             }
          }
          if (current_activity == 14) {
             if (current_bank > 0) {
               current_bank -= 1;
             }
             else current_bank = 3;
          }
    }
    else if (current_activity==2) {
#if defined(BEARING_METHOD_2) || defined(BEARING_METHOD_4)       
               if (home_pos && !home_bear) {

                  home_bearing--;
                  if (home_bearing<0) home_bearing = 359;
               }
#endif     
               if (gps_fix && home_pos && home_bear) {
                  current_activity = 0;
                }
    }
   else if (current_activity==1 && home_pos && home_bear) {
          home_bearing--;
   }   
   
  }
}


//right button
void rightButtonReleaseEvents(Button &btn)
{
  if ( right_button.holdTime() < 1000 ) {
     
    if (current_activity==0) {
        displaymenu.next();
    }
    else if ( current_activity != 0 && current_activity != 1 && current_activity != 2 ) {
              //We're in a setting area: Right button decrase current value.
          if (current_activity == 3) servoconf_tmp[0]++;		 
          if (current_activity == 4) configuration.pan_minangle++;
          if (current_activity == 5) servoconf_tmp[1]++;
          if (current_activity == 6) configuration.pan_maxangle++;
          if (current_activity == 7) servoconf_tmp[2]++;
          if (current_activity == 8) configuration.tilt_minangle++;        
          if (current_activity == 9) servoconf_tmp[3]++;
          if (current_activity == 10) configuration.tilt_maxangle++;
           if (current_activity == 12) {
            if (configuration.telemetry < 3) {
               configuration.telemetry += 1;
            }
          }
          if (current_activity == 13) {
             if (configuration.baudrate < 7) {
               configuration.baudrate += 1;
             }       
          }
          if (current_activity == 14) {
             if (current_bank < 3) {
               current_bank += 1;
             }
             else current_bank = 0;
          }
    }
    else if (current_activity==2) {

#if defined(BEARING_METHOD_2)  || defined(BEARING_METHOD_4) 
           if (home_pos && !home_bear) {
                  home_bearing++;
                          if (home_bearing>359) home_bearing = 0;
               }
#endif    
           if (gps_fix && home_pos && home_bear) {
              // reset home pos
              home_pos = false;
              home_bear = false; 
           }
    }
     else if (current_activity==1 && home_pos && home_bear) {
          home_bearing++;
   }
   
  }
}

//########################################################### MENU #######################################################################################

void init_menu() {
	rootMenu.add_item(&m1i1Item, &screen_tracking); //start track
	rootMenu.add_item(&m1i2Item, &screen_sethome); //set home position
	rootMenu.add_menu(&m1m3Menu); //configure
		m1m3Menu.add_menu(&m1m3m1Menu); //config servos
			m1m3m1Menu.add_menu(&m1m3m1m1Menu); //config pan
				m1m3m1m1Menu.add_item(&m1m3m1m1l1Item, &configure_pan_minpwm); // pan min pwm
				m1m3m1m1Menu.add_item(&m1m3m1m1l2Item, &configure_pan_maxpwm); // pan max pwm
                                m1m3m1m1Menu.add_item(&m1m3m1m1l3Item, &configure_pan_minangle); // pan min angle
				m1m3m1m1Menu.add_item(&m1m3m1m1l4Item, &configure_pan_maxangle); // pan max angle
			m1m3m1Menu.add_menu(&m1m3m1m2Menu); //config tilt
				m1m3m1m2Menu.add_item(&m1m3m1m2l1Item, &configure_tilt_minpwm); // tilt min pwm
				m1m3m1m2Menu.add_item(&m1m3m1m2l2Item, &configure_tilt_maxpwm); // tilt max pwm
                                m1m3m1m2Menu.add_item(&m1m3m1m2l3Item, &configure_tilt_minangle); // tilt min angle
				m1m3m1m2Menu.add_item(&m1m3m1m2l4Item, &configure_tilt_maxangle); // tilt max angle
                        m1m3m1Menu.add_item(&m1m3m1i3Item, &configure_test_servo);
                m1m3Menu.add_item(&m1m3i2Item, &configure_telemetry); // select telemetry protocol ( Teensy++2 only ) 
                m1m3Menu.add_item(&m1m3i3Item, &configure_baudrate); // select telemetry protocol
        rootMenu.add_item(&m1i4Item, &screen_bank); //set home position
	displaymenu.set_root_menu(&rootMenu);
}

//######################################## MENUS #####################################################################

void display_menu() {
        Menu const* displaymenu_current = displaymenu.get_current_menu();
	MenuComponent const* displaymenu_sel = displaymenu_current->get_selected();

        for (int n = 1; n < 5; ++n) {
          char string_buffer[21];
	//
            if ( (displaymenu_current->get_num_menu_components()) >= n ) {
                
      		  MenuComponent const* displaymenu_comp = displaymenu_current->get_menu_component(n-1);
      		  String getname = displaymenu_comp->get_name();
			  for ( int l = getname.length()-1 ; l<20 ; l++ ) {
				getname = getname + " ";
				}			  

      		  if (displaymenu_sel == displaymenu_comp) {
 
      				getname.setCharAt(19,'<');
      		  } else {
      		  		getname.setCharAt(19, ' ');
      		  }
      	
      		  getname.toCharArray(string_buffer,21);
                
            }
            else {
               //empty_line.toCharArray(string_buffer,21);
               string_load2.copy(string_buffer);
            }
		store_lcdline(n, string_buffer);
		
	};
		
}

//menu item callback functions

void screen_tracking(MenuItem* p_menu_item) {
	
	current_activity = 1;
}

void screen_sethome(MenuItem* p_menu_item) {
	current_activity = 2;
}

void configure_pan_minpwm(MenuItem* p_menu_item) {
	current_activity = 3;
}

void configure_pan_minangle(MenuItem* p_menu_item) {
	current_activity = 4;
}

void configure_pan_maxpwm(MenuItem* p_menu_item) {
	current_activity = 5;
}

void configure_pan_maxangle(MenuItem* p_menu_item) {
	current_activity = 6;
}

void configure_tilt_minpwm(MenuItem* p_menu_item) {
	current_activity = 7;
}

void configure_tilt_minangle(MenuItem* p_menu_item) {
	current_activity = 8;
}

void configure_tilt_maxpwm(MenuItem* p_menu_item) {
	current_activity = 9;
}

void configure_tilt_maxangle(MenuItem* p_menu_item) {
	current_activity = 10;
}

void configure_test_servo(MenuItem* p_menu_item) {
  
       current_activity = 11;
}

void configure_telemetry(MenuItem* p_menu_item) {
      current_activity = 12;
}

void configure_baudrate(MenuItem* p_menu_item) {
      current_activity = 13;
}

void screen_bank(MenuItem* p_menu_item) {
	current_activity = 14;
}


//######################################## TELEMETRY FUNCTIONS #############################################
void init_serial() {
    
      SerialPort1.begin(baudrates[configuration.baudrate]);
      //SerialPort1.begin(2400);


#ifdef DEBUG
    Serial.println("Serial initialised"); 
#endif

}

//Preparing adding other protocol
void get_telemetry() {
// if (telemetryMetro.check() == 1) {
   if (millis() - lastpacketreceived > 2000) {
      telemetry_ok = false;
      
     
   }
        
#if defined(PROTOCOL_UAVTALK) // OpenPilot / Taulabs 
   if (configuration.telemetry==0) {
      if (uavtalk_read()) {
         protocol = "UAVT";
      }
   }
#endif

#if defined(PROTOCOL_MSP) // Multiwii
    if (configuration.telemetry==1) {
      msp_read(); 
    }
#endif

#if defined(PROTOCOL_LIGHTTELEMETRY) // Ghettostation light protocol. 
   if (configuration.telemetry==2) {
      ltm_read();
   }
#endif

#if defined(PROTOCOL_MAVLINK) // Ardupilot / PixHawk / Taulabs ( mavlink output ) / Other
    if (configuration.telemetry==3) {
      mavlink_read(); 
    }
#endif
//  }
}

void telemetry_off() {
  //reset uav data
  uav_lat = 0;
  uav_lon = 0;                    
  uav_satellites_visible = 0;     
  uav_fix_type = 0;               
  uav_alt = 0;                    
  uav_groundspeed = 0;
  protocol = "";
  telemetry_ok = false;
  }
  
//######################################## SERVOS #####################################################################


void attach_servo(Servo &s, int p, int min, int max) {
 // called at setup() or after a servo configuration change in the menu
	if (s.attached()) {
	s.detach();
	}
	s.attach(p,min,max);

}



void move_servo(Servo &s, int stype, int a, int mina, int maxa) {

 if (stype == 1) {
		//convert angle for pan to pan servo reference point: 0° is pan_minangle
		if (a<=180) {
			a = mina + a;
		} else if ((a>180) && (a<360-mina)) {
                        //relevant only for 360° configs
			a = a - mina;
                }
                else if ((a>180) && (a>360-mina)) {
                          a = mina - (360-a);
                     
		}
                // map angle to microseconds
                int microsec = map(a, 0, mina+maxa, configuration.pan_minpwm, configuration.pan_maxpwm);

                
                s.writeMicroseconds( microsec );
	 }
  else if (stype == 2){
                
                //map angle to microseconds
                int microsec = map(a, mina, maxa, configuration.tilt_minpwm, configuration.tilt_maxpwm);

	        s.writeMicroseconds( microsec );

	}
	



}

void servoPathfinder(int angle_b, int angle_a){   // ( bearing, elevation )
//find the best way to move pan servo considering 0° reference face toward
	if (angle_b<=180) {
			if ( configuration.pan_maxangle >= angle_b ) {
			//works for all config
                                        //define limits
					if (angle_a <= configuration.tilt_minangle) {
					 // checking if we reach the min tilt limit
						angle_a = configuration.tilt_minangle;
					}
                                        else if (angle_a >configuration.tilt_maxangle) {
                                         //shouldn't happend but just in case
                                              angle_a = configuration.tilt_maxangle; 
                                        }

			}
			 else if ( configuration.pan_maxangle < angle_b ) {
                         //relevant for 180° tilt config only, in case bearing is superior to pan_maxangle
				
				angle_b = 360-(180-angle_b);
                                if (angle_b >= 360) {
                                   angle_b = angle_b - 360;
                                }


                                // invert pan axis 
				if ( configuration.tilt_maxangle >= ( 180-angle_a )) {
					// invert pan & tilt for 180° Pan 180° Tilt config

					angle_a = 180-angle_a;
					
				}
				
				else if (configuration.tilt_maxangle < ( 180-angle_a )) {
				 // staying at nearest max pos
				 angle_a = configuration.tilt_maxangle;
				}
				
				
			 }
	}
	
	else if ( angle_b > 180 )
		if( configuration.pan_minangle > 360-angle_b ) {
		//works for all config
			if (angle_a < configuration.tilt_minangle) {
			// checking if we reach the min tilt limit
						angle_a = configuration.tilt_minangle;
			}
		}
		else if ( configuration.pan_minangle <= 360-angle_b ) {
			angle_b = angle_b - 180;
			if ( configuration.tilt_maxangle >= ( 180-angle_a )) {
				// invert pan & tilt for 180/180 conf
				angle_a = 180-angle_a;
				}
			else if (configuration.tilt_maxangle < ( 180-angle_a)) {
				// staying at nearest max pos
				angle_a = configuration.tilt_maxangle;
		}
	}

	move_servo(pan_servo, 1, angle_b, configuration.pan_minangle, configuration.pan_maxangle);
	move_servo(tilt_servo, 2, angle_a, configuration.tilt_minangle, configuration.tilt_maxangle);
}



void test_servos() {
  servoPathfinder(0, 0);
  
  // testing tilt
  
  for ( int i = 359; i > 180; i--) {
  servoPathfinder(i,(360-i)/6);
  delay(100);
  }
  for ( int i = 181; i < 359; i++) {
  servoPathfinder(i,(360-i)/6);
  delay(100);
  }
    
  for (int i=0; i < 360; i++) {
    servoPathfinder(i, i/4); 
    delay(100);
  }
  for (int i=0; i < 360; i++) {
    servoPathfinder(i, 90-(i/4)); 
    delay(100);
  }
  //finished going back to neutral
    servoPathfinder(0,0);
  
  
}

//######################################## TRACKING #############################################

void antenna_tracking() {
// Tracking general function
    //only move servo if gps has a 3D fix, or standby to last known position.
    if (gps_fix && telemetry_ok) {
	
		int rel_alt = uav_alt - home_alt; // relative altitude to ground in decimeters
		calc_tracking( home_lon, home_lat, uav_lon, uav_lat, rel_alt); //calculate tracking bearing/azimuth
		//set current GPS bearing relative to home_bearing
		
		if(Bearing >= home_bearing){
			Bearing-=home_bearing;
		}
		else
		{
			Bearing+=360-home_bearing;
		}
		// serv command
		if(home_dist>DONTTRACKUNDER) { //don't track when <10m 
			servoPathfinder(Bearing,Elevation);
		}
   } 
}



void calc_tracking(float lon1, float lat1, float lon2, float lat2, int alt) {
  
  
//// (homelon, homelat, uavlon, uavlat, uavalt ) 
//// Return Bearing & Elevation angles in degree
//  float a, tc1, R, c, d, dLat, dLon;
// 
// // converting to radian
  lon1=toRad(lon1);
  lat1=toRad(lat1);
  lon2=toRad(lon2);
  lat2=toRad(lat2);
// 
// 
// //calculating bearing in degree decimal
  Bearing = calc_bearing(lon1,lat1,lon2,lat2);
//
// 
////calculating distance between uav & home
  Elevation = calc_elevation(lon1,lat1,lon2,lat2,alt);
 
}


int calc_bearing(float lon1, float lat1, float lon2, float lat2) {


// bearing calc, feeded in radian, output degrees
	float a;
	//calculating bearing 
	a=atan2(sin(lon2-lon1)*cos(lat2), cos(lat1)*sin(lat2)-sin(lat1)*cos(lat2)*cos(lon2-lon1));
	a=toDeg(a);
	if (a<0) a=360+a;
        int b = (int)round(a);
	return b;
}

int calc_elevation(float lon1, float lat1, float lon2, float lat2, int alt) {
  
// feeded in radian, output in degrees
  float a, el, c, d, R, dLat, dLon;
  //calculating distance between uav & home
  R=63710000.0;    //in decimeters. Earth radius 6371km
  dLat = (lat2-lat1);
  dLon = (lon2-lon1);
  a = sin(dLat/2) * sin(dLat/2) + sin(dLon/2) * sin(dLon/2) * cos(lat1) * cos(lat2);
  c = 2* asin(sqrt(a));  
  d =(R * c);
  home_dist = d/10;
  el=atan((float)alt/d);// in radian
  el=toDeg(el); // in degree
  int b = (int)round(el);
  return b;
}

float toRad(float angle) {
// convert degrees to radians
	angle = angle*0.01745329; // (angle/180)*pi
	return angle;
}

float toDeg(float angle) {
// convert radians to degrees.
	angle = angle*57.29577951;   // (angle*180)/pi
        return angle;
}
	


//######################################## COMPASS #############################################

#if defined(BEARING_METHOD_4)

void retrieve_mag() {

////  HMC5883L compass;
////  compass = HMC5883L(); // Construct a new HMC5883 compass.
////  delay(100);
//  compass.SetScale(1.3); // Set the scale of the compass.
//  compass.SetMeasurementMode(Measurement_Continuous); // Set the measurement mode to Continuous
// Retrieve the raw values from the compass (not scaled).
  MagnetometerRaw raw = compass.ReadRawAxis();
// Retrieved the scaled values from the compass (scaled to the configured scale).
  MagnetometerScaled scaled = compass.ReadScaledAxis();
//
// Calculate heading when the magnetometer is level, then correct for signs of axis.
float heading = atan2(scaled.YAxis, scaled.XAxis);

// Once you have your heading, you must then add your ‘Declination Angle’, which is the ‘Error’ of the magnetic field in your location.
// Find yours here: http://www.magnetic-declination.com/



float declinationAngle = MAGDEC / 1000; 
heading += declinationAngle;

// Correct for when signs are reversed.
if(heading < 0)
heading += 2*PI;

// Check for wrap due to addition of declination.
if(heading > 2*PI)
heading -= 2*PI;

// Convert radians to degrees for readability.
home_bearing = (int)round(heading * 180/M_PI);
}
#endif


//######################################## DEBUG #############################################


#if defined(DEBUG)
void debug() {
  
    if (debugMetro.check() == 1)  {
       Serial.print("activ:");
       Serial.println(current_activity);
       Serial.print("conftelem:");
       Serial.println(configuration.telemetry);
       Serial.print("baud");
       Serial.println(configuration.baudrate);
       Serial.print("lat=");
       Serial.println(uav_lat,7);
       Serial.print("lon=");
       Serial.println(uav_lon,7);
       Serial.print("alt=");
       Serial.print(uav_alt);
       Serial.println("");
       Serial.print("dst=");
       Serial.println(home_dist);
       Serial.print("El:");
       Serial.println(Elevation);
       Serial.print("Be:");
       Serial.println(Bearing);
       Serial.print("Be:");
       Serial.println(home_bearing);
       Serial.print("pitch:");
       Serial.println(uav_pitch);
       Serial.print("roll:");
       Serial.println(uav_roll);
       Serial.print("yaw:");
       Serial.println(uav_heading);
       Serial.print("rbat:");
       Serial.println(uav_bat);
       Serial.print("amp:");
       Serial.println(uav_amp);
       Serial.print("rssi:");
       Serial.println(uav_rssi);
       Serial.print("aspeed:");
       Serial.println(uav_airspeed);
       Serial.print("armed:");
       Serial.println(uav_arm);
       Serial.print("fs:");
       Serial.println(uav_failsafe);
       Serial.print("fmode:");
       Serial.println(uav_flightmode);
            
    }
  
}
#endif

#ifdef SIMUGPS

int radius = 6371000;
int simudist = 800;
int simu=0;
void set_simugps() {
  uav_fix_type = 3;
  uav_satellites_visible = 10;
  home_lat = 48.86917;		
  home_lon =  2.24111;		
  uav_alt =  0;
  home_bearing = 0;
  home_pos = true;
  home_bear = true;
} 
void simulate_gps() {
  if (simugpsMetro.check() == 1) {
    simu++;
    if (simu<360) {
//            uav_lat = (LAT_STEPS * cos(simu)) + home_lat;
//            uav_lon = (LON_STEPS * sin(simu)) + home_lon;
  uav_lat = toDeg(asin(sin(toRad(home_lat)) * cos(simudist / radius) + cos(toRad(home_lat)) * sin(simudist / radius) * cos(toRad(simu))));
  uav_lon = toDeg(toRad(home_lon) + atan2(sin(toRad(simu)) * sin(simudist / radius) * cos(toRad(home_lat)), cos(simudist / radius) - sin(toRad(home_lat)) * sin(toRad(uav_lat))));

    }
    else simu = 0;
  }
}
#endif



