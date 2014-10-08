#include "board.h"
#include "mw.h"

// Navigation with Planes under Development.

#define GPS_UPD_HZ             5        // Set loop time for NavUpdate 5 Hz is enough
#define PITCH_COMP             0.5f     // Compensate throttle relative angle of attack
#define DONT_RESET_HOME_AT_ARM          // Probably incompatible!

// from gps.c
extern int32_t nav_bearing;
extern int32_t wp_distance;
extern PID_PARAM navPID_PARAM;
extern PID_PARAM altPID_PARAM;

/*****************************************/
/*   Settings for FixedWing navigation   */
/*****************************************/

#define SAFE_NAV_ALT        20  // Safe Altitude during climbouts Wings Level below this Alt. (ex. trees & buildings..)
#define SAFE_DECSCEND_ZONE  50  // Radius around home where descending is OK

// Experimental
#define CIRCLE     false        // Simple test to fly a Theoretical circle in Hold Mode function Not verified yet

float NaverrorI;
float AlterrorI;
static int16_t lastAltDiff;
static int16_t lastNavDiff;
static int16_t SpeedBoost;
static int16_t AltHist[GPS_UPD_HZ];     // shift register
static int16_t NavDif[GPS_UPD_HZ];      // shift register

void fw_nav_reset()
{
    uint8_t i;
    NaverrorI = 0;
    AlterrorI = 0;
    lastAltDiff = 0;
    lastNavDiff = 0;
    SpeedBoost = 0;
    for (i = 0; i < GPS_UPD_HZ; i++) {
        AltHist[i] = 0;
        NavDif[i] = 0;
    }
}

void fw_nav(void)
{
    int16_t GPS_Heading = GPS_ground_course;;   // Store current bearing
    int16_t Current_Heading;    // Store current bearing
    int16_t altDiff = 0;
    int16_t RTH_Alt = cfg.D8[PIDPOSR];  // conf.pid[PIDALT].D8;
    int16_t delta[2] = { 0, 0 };        // D-Term
    static int16_t NAV_deltaSum = 0, ALT_deltaSum = 0;
    static int16_t GPS_FwTarget = 0;    // Gps correction for Fixed wing
    static int16_t GPS_AltErr = 0;
    static int16_t NAV_Thro = 0;
    int16_t TX_Thro = rcData[THROTTLE]; // Read and store Throttle pos.

    // Calculated Altitude over home in meters
    int16_t curr_Alt = GPS_altitude - GPS_home[ALT];    // GPS
    int16_t navTargetAlt = GPS_hold[ALT] - GPS_home[ALT];       // Diff from homeAlt.

    // Handles ReSetting RTH alt if rth is enabled to low!
    if (f.CLIMBOUT_FW && curr_Alt < RTH_Alt) {
        GPS_hold[ALT] = GPS_home[ALT] + RTH_Alt;
    }
    // Wrap GPS_Heading 1800
    if (GPS_Heading > 1800) {
        GPS_Heading -= 3600;
    }
    if (GPS_Heading < -1800) {
        GPS_Heading += 3600;
    }
    // Only use MAG if Mag and GPS_Heading aligns
    if (sensors(SENSOR_MAG)) {
        if (abs(heading - (GPS_Heading / 10)) > 10 && GPS_speed > 200) {
            Current_Heading = GPS_Heading / 10;
        } else {
            Current_Heading = heading;
        }
    } else {
        Current_Heading = GPS_Heading / 10;
    }

    // Calculate Navigation errors
    GPS_FwTarget = nav_bearing / 100;
    int16_t navDiff = GPS_FwTarget - Current_Heading;   // Navigation Error
    GPS_AltErr = curr_Alt - navTargetAlt;       //  Altitude error Negative means you're to low

    /************ NavTimer ************/
    static uint32_t gpsTimer = 0;
    static uint16_t gpsFreq = 1000 / GPS_UPD_HZ;        // 5HZ 200ms DT

    if (millis() - gpsTimer >= gpsFreq) {
        gpsTimer = millis();

        // Throttle control
        // Deadpan for throttle at correct Alt.
        if (abs(GPS_AltErr) < 1) {
            // Just cruise along in deadpan.
            NAV_Thro = cfg.cruice_throttle;
        } else {
            // Add AltitudeError  and scale up with a factor to throttle
             NAV_Thro = constrain(cfg.cruice_throttle - (GPS_AltErr * cfg.scaler_throttle), cfg.idle_throttle, cfg.climb_throttle);
        }

        // Reset Climbout Flag when Alt have been reached
        if (f.CLIMBOUT_FW && GPS_AltErr >= 0) {
            f.CLIMBOUT_FW = 0;
        }

        // Climb out before RTH
        if (f.GPS_HOME_MODE) {
            if (f.CLIMBOUT_FW) {
                // Accelerate for ground takeoff Untested feature.....
                //#define TAKEOFF_SPEED 10  // 10 m/s ~36km/h
                //if(curr_Alt < 2 && GPS_speed < TAKEOFF_SPEED*100 ){GPS_AltErr = 0;}

                GPS_AltErr = -(cfg.gps_maxclimb * 10);  // Max climbAngle
                NAV_Thro = cfg.climb_throttle;  // Max Allowed Throttle
                if (curr_Alt < SAFE_NAV_ALT) {
                    // Force climb with Level Wings below safe Alt
                    navDiff = 0;
                }
            }

            if ((GPS_distanceToHome < SAFE_DECSCEND_ZONE) && curr_Alt > RTH_Alt) {
            // Start descend to correct RTH Alt.
                GPS_hold[ALT] = GPS_home[ALT] + RTH_Alt;
            }
        }

        // Always DISARM when Home is within 10 meters if FC is in failsafe.
        if (f.FAILSAFE_RTH_ENABLE) {
            if (GPS_distanceToHome < 10) {
                f.ARMED = 0;
                f.CLIMBOUT_FW = 0;      // Abort Climbout
                GPS_hold[ALT] = GPS_home[ALT] + 5;      // Come down
            }
        }

        if (f.GPS_HOLD_MODE) {
            if (wp_distance < 20 * 100 && CIRCLE)
                navDiff = 0;    // Theoretical circle
        }

        // Filtering of navDiff around home to stop nervous servos
        if (GPS_distanceToHome < 10) {
            navDiff *= 0.1;
        }

        // Wrap Heading 180
        if (navDiff <= -180) {
            navDiff += 360;
        }
        if (navDiff >= +180) {
            navDiff -= 360;
        }
        if (abs(navDiff) > 170) {
            navDiff = 175;      // Force a left turn.
        }

        /******************************
        PID for Navigating planes.
        ******************************/
        float NavDT;
        static uint32_t nav_loopT;
        NavDT = (float) (millis() - nav_loopT) / 1000;
        nav_loopT = millis();
        /****************************************************************************************************/
        // Altitude PID

        if (abs(GPS_AltErr) <= 3) {
            // Remove I-Term in deadspan
            AlterrorI *= NavDT;
        }

        GPS_AltErr *= 10;
        AlterrorI += (GPS_AltErr * altPID_PARAM.kI) * NavDT;    // Acumulate I from PIDPOSR
        AlterrorI = constrain(AlterrorI, -500, 500);    // limits I term influence

        delta[0] = (GPS_AltErr - lastAltDiff);
        lastAltDiff = GPS_AltErr;
        if (abs(delta[0]) > 100)
            delta[0] = 0;
        uint8_t i = 0;
        for (i = 0; i < GPS_UPD_HZ; i++) {
            AltHist[i] = AltHist[i + 1];
        }
        AltHist[GPS_UPD_HZ - 1] = delta[0];

        // Store 1 sec history for D-term in shift register
        ALT_deltaSum = 0;       // Sum History
        for (i = 0; i < GPS_UPD_HZ; i++) {
            ALT_deltaSum += AltHist[i];
        }

        ALT_deltaSum = (ALT_deltaSum * altPID_PARAM.kD) / NavDT;
        altDiff = GPS_AltErr * altPID_PARAM.kP; // Add P in Elevator compensation.
        altDiff += (AlterrorI); // Add I
        /****************************************************************************************************/
        // Nav PID NAV
        if (abs(navDiff) <= 3) {
            NaverrorI *= NavDT; // Remove I-Term in deadspan
        }
        navDiff *= 10;

        NaverrorI += (navDiff * navPID_PARAM.kI) * NavDT;
        NaverrorI = constrain(NaverrorI, -500, 500);

        delta[1] = (navDiff - lastNavDiff);
        lastNavDiff = navDiff;
        if (abs(delta[1]) > 100) {
            delta[1] = 0;
        }
        // Store 1 sec history for D-term in shift register
        for (i = 0; i < GPS_UPD_HZ; i++) {
            NavDif[i] = NavDif[i + 1];
        }
        NavDif[GPS_UPD_HZ - 1] = delta[1];

        NAV_deltaSum = 0;       // Sum History
        for (i = 0; i < GPS_UPD_HZ; i++) {
            NAV_deltaSum += NavDif[i];
        }

        NAV_deltaSum = (NAV_deltaSum * navPID_PARAM.kD) / NavDT;        // Add D

        navDiff *= navPID_PARAM.kP;     // Add P
        navDiff += NaverrorI;   // Add I
        /****************************************************************************************************/

        /******* End of PID *******/

        // Limit outputs
        GPS_angle[PITCH] = constrain(altDiff / 10, -cfg.gps_maxclimb * 10, cfg.gps_maxdive * 10) + ALT_deltaSum;
        GPS_angle[ROLL] = constrain(navDiff / 10, -cfg.gps_maxcorr * 10, cfg.gps_maxcorr * 10) + NAV_deltaSum;
        GPS_angle[YAW] = constrain(navDiff / 10, -cfg.gps_rudder * 10, cfg.gps_rudder * 10) + NAV_deltaSum;

        //***********************************************//
        // Elevator compensation depending on behaviour. //
        //***********************************************//
        // Prevent stall with Disarmed motor  New TEST
        if (f.MOTORS_STOPPED) {
            GPS_angle[PITCH] = constrain(GPS_angle[PITCH], 0, cfg.gps_maxdive * 10);
        }
        // Add elevator compared with rollAngle
        GPS_angle[PITCH] -= abs(angle[ROLL]);

        //***********************************************//
        // Throttle compensation depending on behaviour. //
        //***********************************************//
        // Compensate throttle with pitch Angle
        NAV_Thro -= constrain(angle[PITCH] * PITCH_COMP, 0, 450);
        NAV_Thro = constrain(NAV_Thro, cfg.idle_throttle, cfg.climb_throttle);

        // Force the Plane move forward in headwind with SpeedBoost
#define GPS_MINSPEED  500       // 500= ~18km/h
#define I_TERM        0.1f
        int16_t groundSpeed = GPS_speed;

        int spDiff = (GPS_MINSPEED - groundSpeed) * I_TERM;
        if (GPS_speed < GPS_MINSPEED - 50 || GPS_speed > GPS_MINSPEED + 50) {
            SpeedBoost += spDiff;
        }
        SpeedBoost = constrain(SpeedBoost, 0, 500);
        NAV_Thro += SpeedBoost;

        //***********************************************//
    }
    /******* End of NavTimer *******/

    // PassThru for throttle In AcroMode
    if ((!f.ANGLE_MODE && !f.HORIZON_MODE) || (f.PASSTHRU_MODE && !f.FAILSAFE_RTH_ENABLE)) {
        NAV_Thro = TX_Thro;
        GPS_angle[PITCH] = 0;
        GPS_angle[ROLL] = 0;
        GPS_angle[YAW] = 0;
    }
    rcCommand[THROTTLE] = NAV_Thro;
    rcCommand[YAW] += GPS_angle[YAW];
}