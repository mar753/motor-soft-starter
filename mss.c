// Soft start - sterowanie fazowe
// CPU - PIC16F54, XTAL: 4MHz
// Autor: mar753

#include <xc.h>
#define _XTAL_FREQ 4000000
#pragma config WDT=OFF, CP=OFF, OSC=XT

//******| PARAMETRY DO KONFIGURACJI |******
#define SOFT_START_TIME 3000000UL  //czas softstartu w mikrosekundach (3s); 0,5-3s
#define FREQ 50  //czestotliwosc napiecia sieciowego (50Hz); 50/60
//*****************************************

#if (SOFT_START_TIME < 500000) && (SOFT_START_TIME > 3000000)
#error
#endif

#if (FREQ != 50) && (FREQ != 60)
#error
#endif

#define PWM_STEPS 1000 //ilosc krokow PWM (dokladnosc)
#define PERIOD_US (1000000UL/FREQ)
#define HALF_PERIOD_US (PERIOD_US/2)
#define HALF_PERIOD_US_P(x) (((x)*PERIOD_US)/200) //x% czasu trwania polowki okresu napiecia fazowego

#define FINAL_COUNTER_VALUE ((unsigned)(SOFT_START_TIME/256UL)) //wartosc licznika po odmierzeniu zadanego czasu (SOFT_START_TIME)
#define STEP_VALUE ((FINAL_COUNTER_VALUE/PWM_STEPS)) //ilosc zliczen timera dla osiagniecia pojedynczego kroku (+zaokraglenie w gore)
#define MAX_POWER_FACTOR_VALUE (HALF_PERIOD_US_P(80))
#define MAX_TIME_TO_ENABLE_TRIAC (HALF_PERIOD_US_P(85)) //maksymalny czas po wykryciu zbocza narastajacego po jakim zostanie wlaczony triak
#define POWER_FACTOR_INC_VALUE (MAX_POWER_FACTOR_VALUE/PWM_STEPS) //wartosc o jaka bedzie wzrastal wspolczynnik mocy z kazdym powyzszym krokiem

#if MAX_POWER_FACTOR_VALUE > MAX_TIME_TO_ENABLE_TRIAC
#error
#endif

#if PWM_STEPS > 1000 || PWM_STEPS < 100
#error
#endif

#define SINGLE_TIME_UNIT 256

#define TRIAC_RA0_STATUS (PORTA & 1)
#define TRIAC_RA0_EN (PORTA &= ~0x01)
#define TRIAC_RA0_DIS (PORTA |= 0x01)
#define TRIAC_RA3_STATUS (PORTA & 8)
#define TRIAC_RA3_EN (PORTA &= ~0x08)
#define TRIAC_RA3_DIS (PORTA |= 0x08)

#define RA1_STATUS (PORTA & 2)
#define RA2_STATUS (PORTA & 4)

#define REL_ON (PORTB |= 0x10)
#define LED_ON (PORTB |= 0x20)
#define LED_TOGGLE (PORTB ^= 0x20)

//status flags
#define START_INCREASING_POWER 0x20
#define PREV_STATE_RA1 0x10
#define PREV_STATE_RA2 0x08
#define COUNT1 0x04
#define COUNT2 0x02
#define OK 0x01


unsigned char statusFlags = 0b00000001;
//bits:
//7 6 5                     4             3             2       1       0
//    startIncreasingPower  prevStateRA2  prevStateRA1  count2  count1  ok

unsigned char counter = 0;
unsigned char counter2 = 0;
unsigned char counter3 = 0;
unsigned int counter4 = 0; //zlicza do 65535; odmierza czas do przerwania glownej petli
unsigned char counter5 = STEP_VALUE;
unsigned char counter6 = 0; //przedluzanie impulsu na triak
unsigned char prevTimerValue = 0;
unsigned int powerFactor = 0; //od 0 do 7000 dla 50Hz
unsigned char freezeTMR0 = 0;
unsigned int tmp = 0;

    
void main(void){
    
    TRISA = 0xF6; //port RA1, RA2 jako wejscie; RA0, RA3 - wyjscie
    PORTA = 0x09; //RA0, RA3 stan wysoki;

    TRISB = 0xCF; //port RB4, RB5 jako wyjscie
    PORTB = 0x00; //zgas diode, przekaznik wylaczony

    OPTION = 0x07; //preskaler timera 1:256; inkrementacja timera co 256us

 //******************************************
 //******************************************

    //I etap
    //sprawdzenie obecnosci faz

    //sprawdz odstep miedzy impulsami z RA2 do RA1
    //czyli: czekaj na stan niski na RA2,
    //czekaj na stan niski na RA1 oraz jednoczesnie na stan wysoki RA2
    //(powinno to trwac okolo 2/3 ms dla 50Hz w sieci)

    TMR0 = 0;
    
    //czekaj na stan wysoki na RA1 i jednoczesnie ma byc stan niski na RA2
    while((!RA1_STATUS) || RA2_STATUS){
        if(TMR0 == 255){
            counter++;
            TMR0 = 0;
        }
        if(counter)  //timeout
            break;
    }

    //czekaj na stan niski na RA1 i jednoczesnie ma byc stan wysoki na RA2
    while(RA1_STATUS || (!RA2_STATUS)){
        if(TMR0 == 255){
            counter++;
            TMR0 = 0;
        }
        if(counter)  //timeout
            break;
    }
   
    //czekaj na stan wysoki RA1
    while(!RA1_STATUS){
        if(TMR0 == 255){
            counter++;
            TMR0 = 0;
        }
        if(counter)  //timeout
            break;
    }

    //czekaj na stan niski RA1
    while(RA1_STATUS){
        if(TMR0 == 255){
            counter++;
            TMR0 = 0;
        }
        if(counter)  //timeout
            break;
    }

    TMR0 = 0;
    //dopoki nie minelo .5ms
    while(TMR0 < 2){
        if(!RA1_STATUS)//sprawdzenie drugiej fazy - jesli podczas ok. 500us pojawi sie stan niski na obu wejsciach przerywamy
            if(!RA2_STATUS)
                counter = 1;//konczymy
        if(TMR0 == 255){
            counter++;
            TMR0 = 0;
        }
        if(counter)  //timeout
            break;
    }

    if(counter){
        statusFlags &= ~OK; //nie ok
    }

    counter = 0;

//******************************************
//******************************************

    //jesli ok - etap II
    //jesli nie - mrugamy dioda 1s
    if(statusFlags & OK){
        statusFlags |= PREV_STATE_RA1;
        statusFlags |= PREV_STATE_RA2;

        TMR0 = 0;

        //II etap - soft start
        //mrugamy dioda co 200ms
        //W zaleznosci od ustawien 0,5-3s rozruchu
        //odczytujemy z RA1 i RA2 na zboczu narastajacym wartosc
        //uruchamiamy timer ktory odmierza czas ~8.5ms (85% polowki przebiegu)
        //wlaczamy regularnie danego triaka impulsem (osobno kanal 1 jak i 2)
        //stopniowo zwiekszamy moc poprzez podawanie na triak impulsu wczesniej
        while(1){

//******************************************

            if(freezeTMR0 == 254){
                counter++;
                TMR0 = 0;
                freezeTMR0 = 0;
                prevTimerValue = 254;
            }

//******************************************

            //mrugamy dioda co 200ms
            if(counter == 3){
                LED_TOGGLE;
                counter = 0;
            }
  
//******************************************

            //warunek spelniony gdy timer zwiekszy sie o 1
            if(freezeTMR0 != prevTimerValue){

                //nie zwiekszamy mocy dopoki nie uruchomimy po raz pierwszy triaka
                if(statusFlags & START_INCREASING_POWER)
                {
                    //zwiekszamy moc z kazdym krokiem
                    if((counter5++ == STEP_VALUE)){
                        powerFactor += POWER_FACTOR_INC_VALUE;
                        counter5 = 0;
                    }
                }

                counter4++;
                if(!counter4)//jesli przepelnienie
                    counter4--;

                //na wszelki wypadek
                if(powerFactor > MAX_POWER_FACTOR_VALUE)
                    powerFactor = MAX_POWER_FACTOR_VALUE;
                
                prevTimerValue = freezeTMR0;

                if(statusFlags & COUNT1)
                    counter2++;
                if(statusFlags & COUNT2)
                    counter3++;

                //przerywamy impuls na triaki (bedzie on trwal ok. 256us*3)
                if(!TRIAC_RA3_STATUS)
                    if(++counter6 == 3){
                        TRIAC_RA3_DIS;
                        counter6 = 0;
                    }                                      
                if(!TRIAC_RA0_STATUS)
                    if(++counter6 == 3){
                        TRIAC_RA0_DIS;
                        counter6 = 0;
                    }
            }

//******************************************

            //wykrywanie zbocza narastajacego na RA1 i RA2
            if(!RA1_STATUS) //jesli stan niski
                statusFlags &= ~PREV_STATE_RA1;
            if(!RA2_STATUS) //jesli stan niski
                statusFlags &= ~PREV_STATE_RA2;

            if(!(statusFlags & PREV_STATE_RA1) && RA1_STATUS){
                statusFlags |= COUNT1;            
                statusFlags |= START_INCREASING_POWER; //uruchamiamy takze zwiekszanie sie mocy
            }
            if(!(statusFlags & PREV_STATE_RA2) && RA2_STATUS){
                statusFlags |= COUNT2;
                statusFlags |= START_INCREASING_POWER; //uruchamiamy takze zwiekszanie sie mocy
            }
            
//******************************************

            tmp = (unsigned)MAX_TIME_TO_ENABLE_TRIAC - powerFactor;

            //impuls na triaka L1 (stopniowo coraz wczesniej)
            if((counter2*SINGLE_TIME_UNIT) > tmp)
            {
                TRIAC_RA0_EN;  //wlacz triaka RA0 (L1)
                counter2 = 0;  //reset
                statusFlags |= PREV_STATE_RA1;
                statusFlags &= ~COUNT1;
            }
            
            //impuls na triaka L3 (stopniowo coraz wczesniej)
            if((counter3*SINGLE_TIME_UNIT) > tmp)
            {
                TRIAC_RA3_EN;  //wlacz triaka RA3 (L3)
                counter3 = 0;  //reset
                statusFlags |= PREV_STATE_RA2;
                statusFlags &= ~COUNT2;
            }
            
//******************************************

            //jesli czas przeznaczony na uruchomienie zostal osiagniety, konczymy
            if(counter4 == FINAL_COUNTER_VALUE)
                break;
         
            freezeTMR0 = TMR0; //zamrazamy wartosc timera (zeby nie zmienila nam sie podczas wykonywania petli)
        }

//******************************************
//******************************************
        
        //III etap
        //zostawiamy RA0 i RA3 w stanie niskim (wlaczamy triaki na stale)
        //wlaczamy diode na stale
        //wlaczamy stycznik
        TRIAC_RA0_EN;
        TRIAC_RA3_EN;
        LED_ON;
        REL_ON;
    }

//******************************************

    //jesli blad: brak fazy, przerwany przewod, mrugamy dioda co ok. 1s
    else{ 
        while(1){
            if(TMR0 == 254){
                counter++;
                TMR0 = 0;
            }
            if(counter == 15){
                LED_TOGGLE;
                counter = 0;
            }
        }
    }

//******************************************
    
    while(1);
}
