/*---------------------------------------------------
HTTP 1.1 Temperature & Humidity Webserver for ESP8266 
and ext value for Raspberry Thermostat

by Jpnos 2017  - free for everyone

 original developer :
by Stefan Thesen 05/2015 - free for anyone

Connect DHT21 / AMS2301 at GPIO2
---------------------------------------------------*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "time_ntp.h"
#include "DHT.h"
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>

// WiFi connection
const char* ssid = "wifi-name";
const char* password = "wifi-pwd";

// ntp timestamp
unsigned long ulSecs2000_timer=0;

// storage for Measurements; keep some mem free; allocate remainder
#define KEEP_MEM_FREE 10240
#define MEAS_SPAN_H 24
unsigned long ulMeasCount=0;    // values already measured
unsigned long ulNoMeasValues=0; // size of array
unsigned long ulMeasDelta_ms;   // distance to next meas time
unsigned long ulNextMeas_ms;    // next meas time
unsigned long *pulTime;         // array for time points of measurements
float *pfTemp,*pfHum;           // array for temperature and humidity measurements

unsigned long ulReqcount;       // how often has a valid page been requested
unsigned long ulReconncount;    // how often did we connect to WiFi
float dhtTemp;			// dht read temperature
float dhtUm;			// dht read umidity
// Create an instance of the server on Port 80
WiFiServer server(80);

//////////////////////////////
// DHT21 / AMS2301 is at GPIO2
//////////////////////////////
#define DHTPIN 2

// Uncomment whatever type you're using!
//#define DHTTYPE DHT11   // DHT 11 
#define DHTTYPE DHT22   // DHT 22  (AM2302)
//#define DHTTYPE DHT21   // DHT 21 (AM2301)

// init DHT; 3rd parameter = 16 works for ESP8266@80MHz
DHT dht(DHTPIN, DHTTYPE,16); 

// needed to avoid link error on ram check
extern "C" 
{
#include "user_interface.h"
}

/////////////////////
// the setup routine
/////////////////////
void setup() 
{
  // setup globals
  ulReqcount=0; 
  ulReconncount=0;
    
  // start serial
  Serial.begin(115200);
  Serial.println("Esp8266 WI-FI Temp/Umidita Jpnos-2017 v1.0");
  
  // inital connect
  WiFi.mode(WIFI_STA);
  WiFiStart();
  
  // allocate ram for data storage
  uint32_t free=system_get_free_heap_size() - KEEP_MEM_FREE;
  ulNoMeasValues = free / (sizeof(float)*2+sizeof(unsigned long));  // humidity & temp --> 2 + time
  pulTime = new unsigned long[ulNoMeasValues];
  pfTemp = new float[ulNoMeasValues];
  pfHum = new float[ulNoMeasValues];
  
  if (pulTime==NULL || pfTemp==NULL || pfHum==NULL)
  {
    ulNoMeasValues=0;
    Serial.println("Errore in allocazione di memoria!");
  }
  else
  {
    Serial.print("Memoria Preparata per ");
    Serial.print(ulNoMeasValues);
    Serial.println(" data points.");
    
    float fMeasDelta_sec = MEAS_SPAN_H*3600./ulNoMeasValues;
    ulMeasDelta_ms = ( (unsigned long)(fMeasDelta_sec+60) ) * 1000;  // round to full sec
    Serial.print("La misura avviene ogni");
    Serial.print(ulMeasDelta_ms);
    Serial.println(" ms.");
    
    ulNextMeas_ms = millis()+ulMeasDelta_ms;
  }
  
}


///////////////////
// (re-)start WiFi
///////////////////
void WiFiStart()
{
  ulReconncount++;
  
  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Conneetto a ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connessa");
  
  // Start the server
  server.begin();
  Serial.println("Server inizializzato");

  // Print the IP address
  Serial.println(WiFi.localIP());
  
  ///////////////////////////////
  // connect to NTP and get time
  ///////////////////////////////
  ulSecs2000_timer=getNTPTimestamp();
  Serial.print("Ora Corrente da server NTP : " );
  Serial.println(epoch_to_string(ulSecs2000_timer).c_str());

  ulSecs2000_timer -= millis()/1000;  // keep distance to millis() counter

	//////////////////////////////////
  	// OTA UPDATE
  	/////////////////////////////////
  	// Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("thermo-esp8266");

  // No authentication by default
  //ArduinoOTA.setPassword((const char *)"5622");

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  
  
  Serial.println("OTA Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}


/////////////////////////////////////
// make html table for measured data
/////////////////////////////////////
unsigned long MakeTable (WiFiClient *pclient, bool bStream)
{
  unsigned long ulLength=0;
  
  // here we build a big table.
  // we cannot store this in a string as this will blow the memory   
  // thus we count first to get the number of bytes and later on 
  // we stream this out
  if (ulMeasCount==0) 
  {
    String sTable = "Non ci sono dati .<BR>";
    if (bStream)
    {
      pclient->print(sTable);
    }
    ulLength+=sTable.length();
  }
  else
  { 
    unsigned long ulEnd;
    if (ulMeasCount>ulNoMeasValues)
    {
      ulEnd=ulMeasCount-ulNoMeasValues;
    }
    else
    {
      ulEnd=0;
    }
    
    String sTable;
    sTable = "<table style=\"width:800px; margin:0 auto\"><tr><th>tempo</th><th>T &deg;C</th><th>Humidita &#037;</th></tr>";
    sTable += "<style>table, th, td {border: 2px solid black; border-collapse: collapse;} th, td {padding: 5px;} th {text-align: left;}</style>";
    for (unsigned long li=ulMeasCount;li>ulEnd;li--)
    {
      unsigned long ulIndex=(li-1)%ulNoMeasValues;
      sTable += "<tr><td>";
      sTable += epoch_to_string(pulTime[ulIndex]).c_str();
      sTable += "</td><td>";
      sTable += pfTemp[ulIndex];
      sTable += "</td><td>";
      sTable += pfHum[ulIndex];
      sTable += "</td></tr>";

      // play out in chunks of 1k
      if(sTable.length()>1024)
      {
        if(bStream)
        {
          pclient->print(sTable);
          //pclient->write(sTable.c_str(),sTable.length());
        }
        ulLength+=sTable.length();
        sTable="";
      }
    }
    
    // remaining chunk
    sTable+="</table>";
    ulLength+=sTable.length();
    if(bStream)
    {
      pclient->print(sTable);
      //pclient->write(sTable.c_str(),sTable.length());
    }   
  }
  
  return(ulLength);
}
  

////////////////////////////////////////////////////
// make google chart object table for measured data
////////////////////////////////////////////////////
unsigned long MakeList (WiFiClient *pclient, bool bStream)
{
  unsigned long ulLength=0;
  
  // here we build a big list.
  // we cannot store this in a string as this will blow the memory   
  // thus we count first to get the number of bytes and later on 
  // we stream this out
  if (ulMeasCount>0) 
  { 
    unsigned long ulBegin;
    if (ulMeasCount>ulNoMeasValues)
    {
      ulBegin=ulMeasCount-ulNoMeasValues;
    }
    else
    {
      ulBegin=0;
    }
    
    String sTable="";
    for (unsigned long li=ulBegin;li<ulMeasCount;li++)
    {
      // result shall be ['18:24:08 - 21.5.2015',21.10,49.00],
      unsigned long ulIndex=li%ulNoMeasValues;
      sTable += "['";
      sTable += epoch_to_string(pulTime[ulIndex]).c_str();
      sTable += "',";
	  sTable += pfTemp[ulIndex];
      sTable += ",";
      sTable += pfHum[ulIndex];
      sTable += "],\n";

      // play out in chunks of 1k
      if(sTable.length()>1024)
      {
        if(bStream)
        {
          pclient->print(sTable);
          //pclient->write(sTable.c_str(),sTable.length());
        }
        ulLength+=sTable.length();
        sTable="";
      }
    }
    
    // remaining chunk
    if(bStream)
    {
      pclient->print(sTable);
      //pclient->write(sTable.c_str(),sTable.length());
    } 
    ulLength+=sTable.length();  
  }
  
  return(ulLength);
}


//////////////////////////
// create HTTP 1.1 header
//////////////////////////
String MakeHTTPHeader(unsigned long ulLength)
{
  String sHeader;
  
  sHeader  = F("HTTP/1.1 200 OK\r\nContent-Length: ");
  sHeader += ulLength;
  sHeader += F("\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
  
  return(sHeader);
}


////////////////////
// make html footer
////////////////////
String MakeHTTPFooter()
{
  String sResponse;
  
  sResponse  = F("<p align=\"center\"><FONT SIZE=-1><BR>Richieste ="); 
  sResponse += ulReqcount;
  sResponse += F(" - Count ="); 
  sResponse += ulReconncount;
  sResponse += F(" - RAM Libera =");
  sResponse += (uint32_t)system_get_free_heap_size();
  sResponse += F(" - Punti max =");
  sResponse += ulNoMeasValues;
  sResponse += F("<BR>Jpnos 2017<BR></p></body></html>");
  
  return(sResponse);
}


/////////////
// main loop
/////////////
void loop() 
{
  
///////////////////
  // do data logging
  ///////////////////
  if (millis()>=ulNextMeas_ms) 
  {
    ulNextMeas_ms = millis()+ulMeasDelta_ms;
	dhtTemp = dht.readTemperature();
	dhtUm	= dht.readHumidity();
	    if (isnan(dhtTemp) || isnan(dhtUm)) 
			{
      		Serial.println("Failed to read from DHT sensor!"); 
		}   
		else
			{
		pfHum[ulMeasCount%ulNoMeasValues] = dhtUm;
    	pfTemp[ulMeasCount%ulNoMeasValues] = dhtTemp;
    	pulTime[ulMeasCount%ulNoMeasValues] = millis()/1000+ulSecs2000_timer;
    
    	Serial.print("Logging Temperature: "); 
    	Serial.print(pfTemp[ulMeasCount%ulNoMeasValues]);
    	Serial.print(" deg Celsius - Humidity: "); 
    	Serial.print(pfHum[ulMeasCount%ulNoMeasValues]);
    	Serial.print("% - Time: ");
    	Serial.println(pulTime[ulMeasCount%ulNoMeasValues]);
    
    	ulMeasCount++;
		}
	
  }
  
  //////////////////////////////
  // check if WLAN is connected
  //////////////////////////////
  if (WiFi.status() != WL_CONNECTED)
  {
    WiFiStart();
  }

  ///////////////////////////////////
  //OTA 
  //////////////////////////////////
  ArduinoOTA.handle();
  
  ///////////////////////////////////
  // Check if a client has connected
  ///////////////////////////////////
  WiFiClient client = server.available();
  if (!client) 
  {
    return;
  }
  
  // Wait until the client sends some data
  Serial.println("new client");
  unsigned long ultimeout = millis()+250;
  while(!client.available() && (millis()<ultimeout) )
  {
    delay(1);
  }
  if(millis()>ultimeout) 
  { 
    Serial.println("client connection time-out!");
    return; 
  }
  
  /////////////////////////////////////
  // Read the first line of the request
  /////////////////////////////////////
  String sRequest = client.readStringUntil('\r');
  //Serial.println(sRequest);
  client.flush();
  
  // stop client, if request is empty
  if(sRequest=="")
  {
    Serial.println("empty request! - stopping client");
    client.stop();
    return;
  }
  
  // get path; end of path is either space or ?
  // Syntax is e.g. GET /?show=1234 HTTP/1.1
  String sPath="",sParam="", sCmd="";
  String sGetstart="GET ";
  int iStart,iEndSpace,iEndQuest;
  iStart = sRequest.indexOf(sGetstart);
  if (iStart>=0)
  {
    iStart+=+sGetstart.length();
    iEndSpace = sRequest.indexOf(" ",iStart);
    iEndQuest = sRequest.indexOf("?",iStart);
    
    // are there parameters?
    if(iEndSpace>0)
    {
      if(iEndQuest>0)
      {
        // there are parameters
        sPath  = sRequest.substring(iStart,iEndQuest);
        sParam = sRequest.substring(iEndQuest,iEndSpace);
      }
      else
      {
        // NO parameters
        sPath  = sRequest.substring(iStart,iEndSpace);
      }
    }
  }
    
  
  ///////////////////////////
  // format the html response
  ///////////////////////////
  String sResponse,sResponse2,sHeader;
  
  /////////////////////////////
  // format the html page for /
  /////////////////////////////
  if(sPath=="/") 
  {
    ulReqcount++;
    int iIndex= (ulMeasCount-1)%ulNoMeasValues;
    sResponse  = F("<html>\n<head>\n<title>Wi-FI Logger per Temperatura e Umidita</title>\n<script type=\"text/javascript\" src=\"https://www.google.com/jsapi?autoload={'modules':[{'name':'visualization','version':'1','packages':['gauge']}]}\"></script>\n<script type=\"text/javascript\">\nvar temp=");
    sResponse += pfTemp[iIndex];
    sResponse += F(",hum=");
    sResponse += pfHum[iIndex];
    sResponse += F(";\ngoogle.load('visualization', '1', {packages: ['gauge']});google.setOnLoadCallback(drawgaugetemp);google.setOnLoadCallback(drawgaugehum);\nvar gaugetempOptions = {min: -20, max: 50, yellowFrom: -20, yellowTo: 0,redFrom: 30, redTo: 50, minorTicks: 10, majorTicks: ['-20','-10','0','10','20','30','40','50']};\n");
    sResponse += F("var gaugehumOptions = {min: 0, max: 100, yellowFrom: 0, yellowTo: 25, redFrom: 75, redTo: 100, minorTicks: 10, majorTicks: ['0','10','20','30','40','50','60','70','80','90','100']};\nvar gaugetemp,gaugehum;\n\nfunction drawgaugetemp() {\ngaugetempData = new google.visualization.DataTable();\n");
    sResponse += F("gaugetempData.addColumn('number', '\260C');\ngaugetempData.addRows(1);\ngaugetempData.setCell(0, 0, temp);\ngaugetemp = new google.visualization.Gauge(document.getElementById('gaugetemp_div'));\ngaugetemp.draw(gaugetempData, gaugetempOptions);\n}\n\n");
    sResponse += F("function drawgaugehum() {\ngaugehumData = new google.visualization.DataTable();\ngaugehumData.addColumn('number', '%');\ngaugehumData.addRows(1);\ngaugehumData.setCell(0, 0, hum);\ngaugehum = new google.visualization.Gauge(document.getElementById('gaugehum_div'));\ngaugehum.draw(gaugehumData, gaugehumOptions);\n}\n");
    sResponse += F("</script>\n</head>\n<body>\n<font color=\"#000000\"><body bgcolor=\"#d0d0f0\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\">");
    sResponse += F("<p align=\"center\"><FONT SIZE=+3><FONT-WEIGHT=bold>WI-FI Logger per Temperatura e Umidita<BR><FONT SIZE=+1><FONT-WEIGHT=normal>con DHT22 su ESP8266 <BR><FONT SIZE=+1>Ultima Misura alle : ");
    sResponse += epoch_to_string(pulTime[iIndex]).c_str();
    sResponse += F(" <BR></p>\n<div style=\"width:400px; margin:0 auto\">\n<div id=\"gaugetemp_div\" style=\"float:left; width:200px; height: 200px\"></div> \n<div id=\"gaugehum_div\" style=\"float:left; width:200px; height: 200px\"></div>\n<div style=\"clear:both\"></div>\n</div>");
    
    sResponse2 = F("<p align=\"center\">Tabella e Grafici del Logger:<BR><a href=\"/grafico\">Grafico</a>     <a href=\"/tabella\">Tabella</a></p>");
    sResponse2 += MakeHTTPFooter().c_str();
    
    // Send the response to the client 
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()).c_str());
    client.print(sResponse);
    client.print(sResponse2);
  }
  else if(sPath=="/tabella")
  ////////////////////////////////////
  // format the html page for /tabelle
  ////////////////////////////////////
  {
    ulReqcount++;
    unsigned long ulSizeList = MakeTable(&client,false); // get size of table first
    
    sResponse  = F("<html><head><title>WI-FI Logger per Temperatura e Umidita</title></head><body>");
    sResponse += F("<font color=\"#000000\"><body bgcolor=\"#d0d0f0\">");
    sResponse += F("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\">");
    sResponse += F("<p align=\"center\"><FONT SIZE=+3><FONT-WEIGHT=bold>WI-FI Logger per Temperatura e Umidita");
    sResponse += F("<FONT SIZE=+1><FONT-WEIGHT=normal><BR>");
    sResponse += F("<a href=\"/\">Home Page</a><BR><BR>Misurazioni ogni: ");
    sResponse += ulMeasDelta_ms;
    sResponse += F("ms<BR></p>");
    // here the big table will follow later - but let us prepare the end first
      
    // part 2 of response - after the big table
    sResponse2 = MakeHTTPFooter().c_str();
    
    // Send the response to the client - delete strings after use to keep mem low
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()+ulSizeList).c_str()); 
    client.print(sResponse); sResponse="";
    MakeTable(&client,true);
    client.print(sResponse2);
  }
  else if(sPath=="/grafico")
  ///////////////////////////////////
  // format the html page for /grafik
  ///////////////////////////////////
  {
    ulReqcount++;
    unsigned long ulSizeList = MakeList(&client,false); // get size of list first

    sResponse  = F("<html>\n<head>\n<title>WI-FI Logger per Temperatura e Umidita</title>\n<script type=\"text/javascript\" src=\"https://www.google.com/jsapi?autoload={'modules':[{'name':'visualization','version':'1','packages':['corechart']}]}\"></script>\n");
    sResponse += F("<script type=\"text/javascript\"> google.setOnLoadCallback(drawChart);\nfunction drawChart() {var data = google.visualization.arrayToDataTable([\n['Date / Time', 'Temperatura', 'Umidita'],\n");    
    // here the big list will follow later - but let us prepare the end first
      
    // part 2 of response - after the big list
    sResponse2  = F("]);\nvar options = {title: 'Letture',vAxes:{0:{viewWindowMode:'explicit',gridlines:{color:'black'},format:\"##.##\260C\"},1: {gridlines:{color:'transparent'},format:\"##,##%\"},},series:{0:{targetAxisIndex:0},1:{targetAxisIndex:1},},curveType:'none',legend:{ position: 'bottom'}};");
    sResponse2 += F("var chart = new google.visualization.LineChart(document.getElementById('curve_chart'));chart.draw(data, options);}\n</script>\n</head>\n");
    sResponse2 += F("<body>\n<font color=\"#000000\"><body bgcolor=\"#d0d0f0\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\"><p align=\"center\"><FONT SIZE=+3><FONT-WEIGHT=bold>WI-FI Logger per Temperatura e Umidita<BR><FONT SIZE=+1><FONT-WEIGHT=normal><a href=\"/\">Home Page</a><BR></p>");
    sResponse2 += F("<BR>\n<div id=\"curve_chart\" style=\"width: 600px; height: 400px; margin: 0 auto\"></div>");
    sResponse2 += MakeHTTPFooter().c_str();
    
    // Send the response to the client - delete strings after use to keep mem low
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()+ulSizeList).c_str()); 
    client.print(sResponse); sResponse="";
    MakeList(&client,true);
    client.print(sResponse2);
  }
  else if(sPath=="/dati")
  ///////////////////////////////////
  // format the html page for /dati ovvero passo dati in json
  ///////////////////////////////////
  {
    ulReqcount++;
    unsigned long ulSizeList = MakeList(&client,false); // get size of list first
    int iIndex= (ulMeasCount-1)%ulNoMeasValues;
    
    sResponse = "{";
    sResponse += "\"heap\": "+String(ESP.getFreeHeap());
    sResponse += ", \"S_temperature\": "+String(pfTemp[iIndex]);
    sResponse += ", \"S_humidity\": "+String(pfHum[iIndex]);
    sResponse += ", \"S_time\": \"" +epoch_to_string(pulTime[iIndex])+"\"";
    sResponse += "}";
    Serial.print("Json: \n");
    Serial.print(sResponse);
    // Send the response to the client 
    client.print(MakeHTTPHeader(sResponse.length()).c_str());
    client.print(sResponse); sResponse="";
 
  }
  else
  ////////////////////////////
  // 404 for non-matching path
  ////////////////////////////
  {
    sResponse="<html><head><title>404 Not Found</title></head><body><h1>Not Found</h1><p>The requested URL was not found on this server.</p></body></html>";
    
    sHeader  = F("HTTP/1.1 404 Not found\r\nContent-Length: ");
    sHeader += sResponse.length();
    sHeader += F("\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    
    // Send the response to the client
    client.print(sHeader);
    client.print(sResponse);
  }
  
  // and stop the client
  client.stop();
  Serial.println("Client disconnected");
}

