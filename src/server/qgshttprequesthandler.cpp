/***************************************************************************
                              qgshttprequesthandler.cpp
                              -------------------------
  begin                : June 29, 2007
  copyright            : (C) 2007 by Marco Hugentobler
                         (C) 2014 by Alessandro Pasotti
  email                : marco dot hugentobler at karto dot baug dot ethz dot ch
                         a dot pasotti at itopen dot it
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgis.h"
#include "qgshttprequesthandler.h"
#if QT_VERSION < 0x050000
#include "qgsftptransaction.h"
#include "qgshttptransaction.h"
#endif
#include "qgsmessagelog.h"
#include "qgsmapserviceexception.h"
#include <QBuffer>
#include <QByteArray>
#include <QDomDocument>
#include <QFile>
#include <QImage>
#include <QTextStream>
#include <QStringList>
#include <QUrl>
#include <fcgi_stdio.h>


QgsHttpRequestHandler::QgsHttpRequestHandler( const bool captureOutput )
    : QgsRequestHandler()
{
  mException = nullptr;
  mHeadersSent = false;
  mCaptureOutput = captureOutput;
}

QgsHttpRequestHandler::~QgsHttpRequestHandler()
{
  delete mException;
}

void QgsHttpRequestHandler::setHttpResponse( QByteArray *ba, const QString &format )
{
  QgsMessageLog::logMessage( QStringLiteral( "Checking byte array is ok to set..." ) );
  if ( !ba )
  {
    return;
  }

  if ( ba->size() < 1 )
  {
    return;
  }
  QgsMessageLog::logMessage( QStringLiteral( "Byte array looks good, setting response..." ) );
  appendBody( *ba );
  mInfoFormat = format;
}

bool QgsHttpRequestHandler::responseReady() const
{
  return !mHeaders.isEmpty() || !mBody.isEmpty();
}

bool QgsHttpRequestHandler::exceptionRaised() const
{
  return mException;
}

void QgsHttpRequestHandler::setDefaultHeaders()
{
  //format
  QString format = mInfoFormat;
  if ( mInfoFormat.startsWith( QLatin1String( "text/" ) ) || mInfoFormat.startsWith( QLatin1String( "application/vnd.ogc.gml" ) ) )
  {
    format.append( "; charset=utf-8" );
  }
  setHeader( QStringLiteral( "Content-Type" ), format );

  //length
  int contentLength = mBody.size();
  if ( contentLength > 0 ) // size is not known when streaming
  {
    setHeader( QStringLiteral( "Content-Length" ), QString::number( contentLength ) );
  }
}

void QgsHttpRequestHandler::setHeader( const QString &name, const QString &value )
{
  mHeaders.insert( name, value );
}


void QgsHttpRequestHandler::clearHeaders()
{
  mHeaders.clear();
}

int QgsHttpRequestHandler::removeHeader( const QString &name )
{
  return mHeaders.remove( name );
}

void QgsHttpRequestHandler::appendBody( const QByteArray &body )
{
  mBody.append( body );
}

void QgsHttpRequestHandler::clearBody()
{
  mBody.clear();
}


void QgsHttpRequestHandler::setInfoFormat( const QString &format )
{
  mInfoFormat = format;
}

void QgsHttpRequestHandler::addToResponseHeader( const char * response )
{
  if ( mCaptureOutput )
  {
    mResponseHeader.append( response );
  }
  else
  {
    fputs( response, FCGI_stdout );
  }
}

void QgsHttpRequestHandler::addToResponseBody( const char * response )
{
  if ( mCaptureOutput )
  {
    mResponseBody.append( response );
  }
  else
  {
    fputs( response, FCGI_stdout );
  }
}

void QgsHttpRequestHandler::sendHeaders()
{
  // Send default headers if they've not been set in a previous stage
  if ( mHeaders.empty() )
  {
    setDefaultHeaders();
  }

  QMap<QString, QString>::const_iterator it;
  for ( it = mHeaders.constBegin(); it != mHeaders.constEnd(); ++it )
  {
    addToResponseHeader( it.key().toUtf8() );
    addToResponseHeader( ": " );
    addToResponseHeader( it.value().toUtf8() );
    addToResponseHeader( "\n" );
  }
  addToResponseHeader( "\n" );
  mHeaders.clear();
  mHeadersSent = true;
}

void QgsHttpRequestHandler::sendBody()
{
  if ( mCaptureOutput )
  {
    mResponseBody.append( mBody );
  }
  else
  {
    // Cannot use addToResponse because it uses printf
    size_t result = fwrite(( void* )mBody.data(), mBody.size(), 1, FCGI_stdout );
#ifdef QGISDEBUG
    QgsMessageLog::logMessage( QStringLiteral( "Sent %1 blocks of %2 bytes" ).arg( result ).arg( mBody.size() ) );
#else
    Q_UNUSED( result );
#endif
  }
}

#ifdef HAVE_SERVER_PYTHON_PLUGINS
void QgsHttpRequestHandler::setPluginFilters( const QgsServerFiltersMap& pluginFilters )
{
  mPluginFilters = pluginFilters;
}
#endif

void QgsHttpRequestHandler::sendResponse()
{
  QgsMessageLog::logMessage( QStringLiteral( "Sending HTTP response" ) );
  if ( ! responseReady() )
  {
    QgsMessageLog::logMessage( QStringLiteral( "Trying to send out an invalid response" ) );
    return;
  }
#ifdef HAVE_SERVER_PYTHON_PLUGINS
  // Plugin hook
  // Iterate filters and call their sendResponse() method
  QgsServerFiltersMap::const_iterator filtersIterator;
  for ( filtersIterator = mPluginFilters.constBegin(); filtersIterator != mPluginFilters.constEnd(); ++filtersIterator )
  {
    filtersIterator.value()->sendResponse();
  }
#endif
  if ( ! headersSent() )
  {
    sendHeaders();
  }
  sendBody();
  //Clear the body to allow for streaming content to stdout
  clearBody();
}

QPair<QByteArray, QByteArray> QgsHttpRequestHandler::getResponse()
{
  // TODO: check that this is not an evil bug!
  QPair<QByteArray, QByteArray> response( mResponseHeader, mResponseBody );
  return response;
}

QString QgsHttpRequestHandler::formatToMimeType( const QString& format ) const
{
  if ( format.compare( QLatin1String( "png" ), Qt::CaseInsensitive ) == 0 )
  {
    return QStringLiteral( "image/png" );
  }
  else if ( format.compare( QLatin1String( "jpg" ), Qt::CaseInsensitive ) == 0 )
  {
    return QStringLiteral( "image/jpeg" );
  }
  else if ( format.compare( QLatin1String( "svg" ), Qt::CaseInsensitive ) == 0 )
  {
    return QStringLiteral( "image/svg+xml" );
  }
  else if ( format.compare( QLatin1String( "pdf" ), Qt::CaseInsensitive ) == 0 )
  {
    return QStringLiteral( "application/pdf" );
  }
  return format;
}

void QgsHttpRequestHandler::setGetMapResponse( const QString& service, QImage* img, int imageQuality = -1 )
{
  Q_UNUSED( service );
  QgsMessageLog::logMessage( QStringLiteral( "setting getmap response..." ) );
  if ( img )
  {
    bool png16Bit = ( mFormatString.compare( QLatin1String( "image/png; mode=16bit" ), Qt::CaseInsensitive ) == 0 );
    bool png8Bit = ( mFormatString.compare( QLatin1String( "image/png; mode=8bit" ), Qt::CaseInsensitive ) == 0 );
    bool png1Bit = ( mFormatString.compare( QLatin1String( "image/png; mode=1bit" ), Qt::CaseInsensitive ) == 0 );
    bool isBase64 = mFormatString.endsWith( QLatin1String( ";base64" ), Qt::CaseInsensitive );
    if ( mFormat != QLatin1String( "PNG" ) && mFormat != QLatin1String( "JPG" ) && !png16Bit && !png8Bit && !png1Bit )
    {
      QgsMessageLog::logMessage( QStringLiteral( "service exception - incorrect image format requested..." ) );
      setServiceException( QgsMapServiceException( QStringLiteral( "InvalidFormat" ), "Output format '" + mFormatString + "' is not supported in the GetMap request" ) );
      return;
    }

    //store the image in a QByteArray and set it directly
    QByteArray ba;
    QBuffer buffer( &ba );
    buffer.open( QIODevice::WriteOnly );

    // Do not use imageQuality for PNG images
    // For now, QImage expects quality to be a range 0-9 for PNG
    if ( mFormat == QLatin1String( "PNG" ) )
    {
      imageQuality = -1;
    }

    if ( png8Bit )
    {
      QVector<QRgb> colorTable;
      medianCut( colorTable, 256, *img );
      QImage palettedImg = img->convertToFormat( QImage::Format_Indexed8, colorTable, Qt::ColorOnly | Qt::ThresholdDither |
                           Qt::ThresholdAlphaDither | Qt::NoOpaqueDetection );
      palettedImg.save( &buffer, "PNG", imageQuality );
    }
    else if ( png16Bit )
    {
      QImage palettedImg = img->convertToFormat( QImage::Format_ARGB4444_Premultiplied );
      palettedImg.save( &buffer, "PNG", imageQuality );
    }
    else if ( png1Bit )
    {
      QImage palettedImg = img->convertToFormat( QImage::Format_Mono, Qt::MonoOnly | Qt::ThresholdDither |
                           Qt::ThresholdAlphaDither | Qt::NoOpaqueDetection );
      palettedImg.save( &buffer, "PNG", imageQuality );
    }
    else
    {
      img->save( &buffer, mFormat.toUtf8().data(), imageQuality );
    }

    if ( isBase64 )
    {
      ba = ba.toBase64();
    }
    setHttpResponse( &ba, formatToMimeType( mFormat ) );
  }
}

void QgsHttpRequestHandler::setGetCapabilitiesResponse( const QDomDocument& doc )
{
  QByteArray ba = doc.toByteArray();
  setHttpResponse( &ba, QStringLiteral( "text/xml" ) );
}

void QgsHttpRequestHandler::setXmlResponse( const QDomDocument& doc )
{
  QByteArray ba = doc.toByteArray();
  setHttpResponse( &ba, QStringLiteral( "text/xml" ) );
}

void QgsHttpRequestHandler::setXmlResponse( const QDomDocument& doc, const QString& mimeType )
{
  QByteArray ba = doc.toByteArray();
  setHttpResponse( &ba, mimeType );
}

void QgsHttpRequestHandler::setGetFeatureInfoResponse( const QDomDocument& infoDoc, const QString& infoFormat )
{
  QByteArray ba;
  QgsMessageLog::logMessage( "Info format is:" + infoFormat );

  if ( infoFormat == QLatin1String( "text/xml" ) || infoFormat.startsWith( QLatin1String( "application/vnd.ogc.gml" ) ) )
  {
    ba = infoDoc.toByteArray();
  }
  else if ( infoFormat == QLatin1String( "text/plain" ) || infoFormat == QLatin1String( "text/html" ) )
  {
    //create string
    QString featureInfoString;

    if ( infoFormat == QLatin1String( "text/plain" ) )
    {
      featureInfoString.append( "GetFeatureInfo results\n" );
      featureInfoString.append( "\n" );
    }
    else if ( infoFormat == QLatin1String( "text/html" ) )
    {
      featureInfoString.append( "<HEAD>\n" );
      featureInfoString.append( "<TITLE> GetFeatureInfo results </TITLE>\n" );
      featureInfoString.append( "<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\">\n" );
      featureInfoString.append( "</HEAD>\n" );
      featureInfoString.append( "<BODY>\n" );
    }

    QDomNodeList layerList = infoDoc.elementsByTagName( QStringLiteral( "Layer" ) );

    //layer loop
    for ( int i = 0; i < layerList.size(); ++i )
    {
      QDomElement layerElem = layerList.at( i ).toElement();
      if ( infoFormat == QLatin1String( "text/plain" ) )
      {
        featureInfoString.append( "Layer '" + layerElem.attribute( QStringLiteral( "name" ) ) + "'\n" );
      }
      else if ( infoFormat == QLatin1String( "text/html" ) )
      {
        featureInfoString.append( "<TABLE border=1 width=100%>\n" );
        featureInfoString.append( "<TR><TH width=25%>Layer</TH><TD>" + layerElem.attribute( QStringLiteral( "name" ) ) + "</TD></TR>\n" );
        featureInfoString.append( "</BR>" );
      }

      //feature loop (for vector layers)
      QDomNodeList featureNodeList = layerElem.elementsByTagName( QStringLiteral( "Feature" ) );
      QDomElement currentFeatureElement;

      if ( featureNodeList.size() < 1 ) //raster layer?
      {
        QDomNodeList attributeNodeList = layerElem.elementsByTagName( QStringLiteral( "Attribute" ) );
        for ( int j = 0; j < attributeNodeList.size(); ++j )
        {
          QDomElement attributeElement = attributeNodeList.at( j ).toElement();
          if ( infoFormat == QLatin1String( "text/plain" ) )
          {
            featureInfoString.append( attributeElement.attribute( QStringLiteral( "name" ) ) + " = '" +
                                      attributeElement.attribute( QStringLiteral( "value" ) ) + "'\n" );
          }
          else if ( infoFormat == QLatin1String( "text/html" ) )
          {
            featureInfoString.append( "<TR><TH>" + attributeElement.attribute( QStringLiteral( "name" ) ) + "</TH><TD>" +
                                      attributeElement.attribute( QStringLiteral( "value" ) ) + "</TD></TR>\n" );
          }
        }
      }
      else //vector layer
      {
        for ( int j = 0; j < featureNodeList.size(); ++j )
        {
          QDomElement featureElement = featureNodeList.at( j ).toElement();
          if ( infoFormat == QLatin1String( "text/plain" ) )
          {
            featureInfoString.append( "Feature " + featureElement.attribute( QStringLiteral( "id" ) ) + "\n" );
          }
          else if ( infoFormat == QLatin1String( "text/html" ) )
          {
            featureInfoString.append( "<TABLE border=1 width=100%>\n" );
            featureInfoString.append( "<TR><TH>Feature</TH><TD>" + featureElement.attribute( QStringLiteral( "id" ) ) + "</TD></TR>\n" );
          }
          //attribute loop
          QDomNodeList attributeNodeList = featureElement.elementsByTagName( QStringLiteral( "Attribute" ) );
          for ( int k = 0; k < attributeNodeList.size(); ++k )
          {
            QDomElement attributeElement = attributeNodeList.at( k ).toElement();
            if ( infoFormat == QLatin1String( "text/plain" ) )
            {
              featureInfoString.append( attributeElement.attribute( QStringLiteral( "name" ) ) + " = '" +
                                        attributeElement.attribute( QStringLiteral( "value" ) ) + "'\n" );
            }
            else if ( infoFormat == QLatin1String( "text/html" ) )
            {
              featureInfoString.append( "<TR><TH>" + attributeElement.attribute( QStringLiteral( "name" ) ) + "</TH><TD>" + attributeElement.attribute( QStringLiteral( "value" ) ) + "</TD></TR>\n" );
            }
          }

          if ( infoFormat == QLatin1String( "text/html" ) )
          {
            featureInfoString.append( "</TABLE>\n</BR>\n" );
          }
        }
      }
      if ( infoFormat == QLatin1String( "text/plain" ) )
      {
        featureInfoString.append( "\n" );
      }
      else if ( infoFormat == QLatin1String( "text/html" ) )
      {
        featureInfoString.append( "</TABLE>\n<BR></BR>\n" );

      }
    }
    if ( infoFormat == QLatin1String( "text/html" ) )
    {
      featureInfoString.append( "</BODY>\n" );
    }
    ba = featureInfoString.toUtf8();
  }
  else //unsupported format, set exception
  {
    setServiceException( QgsMapServiceException( QStringLiteral( "InvalidFormat" ), "Feature info format '" + infoFormat + "' is not supported. Possibilities are 'text/plain', 'text/html' or 'text/xml'." ) );
    return;
  }

  setHttpResponse( &ba, infoFormat );
}

void QgsHttpRequestHandler::setServiceException( const QgsMapServiceException& ex )
{
  // Safety measure to avoid potential leaks if called repeatedly
  delete mException;
  mException = new QgsMapServiceException( ex );
  //create Exception DOM document
  QDomDocument exceptionDoc;
  QDomElement serviceExceptionReportElem = exceptionDoc.createElement( QStringLiteral( "ServiceExceptionReport" ) );
  serviceExceptionReportElem.setAttribute( QStringLiteral( "version" ), QStringLiteral( "1.3.0" ) );
  serviceExceptionReportElem.setAttribute( QStringLiteral( "xmlns" ), QStringLiteral( "http://www.opengis.net/ogc" ) );
  exceptionDoc.appendChild( serviceExceptionReportElem );
  QDomElement serviceExceptionElem = exceptionDoc.createElement( QStringLiteral( "ServiceException" ) );
  serviceExceptionElem.setAttribute( QStringLiteral( "code" ), ex.code() );
  QDomText messageText = exceptionDoc.createTextNode( ex.message() );
  serviceExceptionElem.appendChild( messageText );
  serviceExceptionReportElem.appendChild( serviceExceptionElem );

  QByteArray ba = exceptionDoc.toByteArray();
  // Clear response headers and body and set new exception
  // TODO: check for headersSent()
  clearHeaders();
  clearBody();
  setHttpResponse( &ba, QStringLiteral( "text/xml" ) );
}

void QgsHttpRequestHandler::setGetPrintResponse( QByteArray* ba )
{
  if ( mFormatString.endsWith( QLatin1String( ";base64" ), Qt::CaseInsensitive ) )
  {
    *ba = ba->toBase64();
  }
  setHttpResponse( ba, formatToMimeType( mFormat ) );
}


bool QgsHttpRequestHandler::startGetFeatureResponse( QByteArray* ba, const QString& infoFormat )
{
  if ( !ba )
  {
    return false;
  }

  if ( ba->size() < 1 )
  {
    return false;
  }

  QString format;
  if ( infoFormat == QLatin1String( "GeoJSON" ) )
    format = QStringLiteral( "text/plain" );
  else
    format = QStringLiteral( "text/xml" );

  setInfoFormat( format );
  sendHeaders();
  appendBody( *ba );
  // Streaming
  sendResponse();
  return true;
}

void QgsHttpRequestHandler::setGetFeatureResponse( QByteArray* ba )
{
  if ( !ba )
  {
    return;
  }

  if ( ba->size() < 1 )
  {
    return;
  }
  appendBody( *ba );
  // Streaming
  sendResponse();
}

void QgsHttpRequestHandler::endGetFeatureResponse( QByteArray* ba )
{
  if ( !ba )
  {
    return;
  }
  appendBody( *ba );
  // Streaming
  sendResponse();
}

void QgsHttpRequestHandler::setGetCoverageResponse( QByteArray* ba )
{
  setHttpResponse( ba, QStringLiteral( "image/tiff" ) );
}

void QgsHttpRequestHandler::requestStringToParameterMap( const QString& request, QMap<QString, QString>& parameters )
{
  parameters.clear();


  //insert key and value into the map (parameters are separated by &)
  Q_FOREACH ( const QString& element, request.split( "&" ) )
  {
    int sepidx = element.indexOf( QLatin1String( "=" ), 0, Qt::CaseSensitive );
    if ( sepidx == -1 )
    {
      continue;
    }

    QString key = element.left( sepidx );
    key = QUrl::fromPercentEncoding( key.toUtf8() ); //replace encoded special characters and utf-8 encodings

    QString value = element.mid( sepidx + 1 );
    value.replace( QLatin1String( "+" ), QLatin1String( " " ) );
    value = QUrl::fromPercentEncoding( value.toUtf8() ); //replace encoded special characters and utf-8 encodings

    if ( key.compare( QLatin1String( "SLD_BODY" ), Qt::CaseInsensitive ) == 0 )
    {
      key = QStringLiteral( "SLD" );
    }
    else if ( key.compare( QLatin1String( "SLD" ), Qt::CaseInsensitive ) == 0 )
    {
#if QT_VERSION < 0x050000
      QByteArray fileContents;
      if ( value.startsWith( "http", Qt::CaseInsensitive ) )
      {
        QgsHttpTransaction http( value );
        if ( !http.getSynchronously( fileContents ) )
        {
          continue;
        }
      }
      else if ( value.startsWith( "ftp", Qt::CaseInsensitive ) )
      {
        Q_NOWARN_DEPRECATED_PUSH;
        QgsFtpTransaction ftp;
        if ( !ftp.get( value, fileContents ) )
        {
          continue;
        }
        value = QUrl::fromPercentEncoding( fileContents );
        Q_NOWARN_DEPRECATED_POP;
      }
      else
      {
        continue; //only http and ftp supported at the moment
      }
      value = QUrl::fromPercentEncoding( fileContents );
#else
      QgsMessageLog::logMessage( QStringLiteral( "http and ftp methods not supported with Qt5." ) );
      continue;
#endif

    }
    parameters.insert( key.toUpper(), value );
    QgsMessageLog::logMessage( "inserting pair " + key.toUpper() + " // " + value + " into the parameter map" );
  }

  //feature info format?
  QString infoFormat = parameters.value( QStringLiteral( "INFO_FORMAT" ) );
  if ( !infoFormat.isEmpty() )
  {
    mFormat = infoFormat;
  }
  else //capabilities format or GetMap format
  {
    mFormatString = parameters.value( QStringLiteral( "FORMAT" ) );
    QString formatString = mFormatString;
    if ( !formatString.isEmpty() )
    {
      QgsMessageLog::logMessage( QStringLiteral( "formatString is: %1" ).arg( formatString ) );

      //remove the image/ in front of the format
      if ( formatString.contains( QLatin1String( "image/png" ), Qt::CaseInsensitive ) || formatString.compare( QLatin1String( "png" ), Qt::CaseInsensitive ) == 0 )
      {
        formatString = QStringLiteral( "PNG" );
      }
      else if ( formatString.contains( QLatin1String( "image/jpeg" ), Qt::CaseInsensitive ) || formatString.contains( QLatin1String( "image/jpg" ), Qt::CaseInsensitive )
                || formatString.compare( QLatin1String( "jpg" ), Qt::CaseInsensitive ) == 0 )
      {
        formatString = QStringLiteral( "JPG" );
      }
      else if ( formatString.compare( QLatin1String( "svg" ), Qt::CaseInsensitive ) == 0 )
      {
        formatString = QStringLiteral( "SVG" );
      }
      else if ( formatString.contains( QLatin1String( "pdf" ), Qt::CaseInsensitive ) )
      {
        formatString = QStringLiteral( "PDF" );
      }

      mFormat = formatString;
    }
  }

}

QString QgsHttpRequestHandler::readPostBody() const
{
  QgsMessageLog::logMessage( QStringLiteral( "QgsHttpRequestHandler::readPostBody" ) );
  char* lengthString = nullptr;
  int length = 0;
  char* input = nullptr;
  QString inputString;
  QString lengthQString;

  lengthString = getenv( "CONTENT_LENGTH" );
  if ( lengthString )
  {
    bool conversionSuccess = false;
    lengthQString = QString( lengthString );
    length = lengthQString.toInt( &conversionSuccess );
    QgsMessageLog::logMessage( "length is: " + lengthQString );
    if ( conversionSuccess )
    {
      input = new char[length + 1];
      memset( input, 0, length + 1 );
      for ( int i = 0; i < length; ++i )
      {
        input[i] = getchar();
      }
      //fgets(input, length+1, stdin);
      if ( input )
      {
        inputString = QString::fromLocal8Bit( input );
      }
      else
      {
        QgsMessageLog::logMessage( QStringLiteral( "input is NULL " ) );
      }
      delete [] input;
    }
    else
    {
      QgsMessageLog::logMessage( QStringLiteral( "could not convert CONTENT_LENGTH to int" ) );
    }
  }
  // Used by the tests
  else if ( getenv( "REQUEST_BODY" ) )
  {
    inputString = getenv( "REQUEST_BODY" );
  }
  return inputString;
}

void QgsHttpRequestHandler::setParameter( const QString &key, const QString &value )
{
  if ( !( key.isEmpty() || value.isEmpty() ) )
  {
    mParameterMap.insert( key, value );
  }
}


QString QgsHttpRequestHandler::parameter( const QString &key ) const
{
  return mParameterMap.value( key );
}

int QgsHttpRequestHandler::removeParameter( const QString &key )
{
  return mParameterMap.remove( key );
}


void QgsHttpRequestHandler::medianCut( QVector<QRgb>& colorTable, int nColors, const QImage& inputImage )
{
  QHash<QRgb, int> inputColors;
  imageColors( inputColors, inputImage );

  if ( inputColors.size() <= nColors ) //all the colors in the image can be mapped to one palette color
  {
    colorTable.resize( inputColors.size() );
    int index = 0;
    QHash<QRgb, int>::const_iterator inputColorIt = inputColors.constBegin();
    for ( ; inputColorIt != inputColors.constEnd(); ++inputColorIt )
    {
      colorTable[index] = inputColorIt.key();
      ++index;
    }
    return;
  }

  //create first box
  QgsColorBox firstBox; //QList< QPair<QRgb, int> >
  int firstBoxPixelSum = 0;
  QHash<QRgb, int>::const_iterator inputColorIt = inputColors.constBegin();
  for ( ; inputColorIt != inputColors.constEnd(); ++inputColorIt )
  {
    firstBox.push_back( qMakePair( inputColorIt.key(), inputColorIt.value() ) );
    firstBoxPixelSum += inputColorIt.value();
  }

  QgsColorBoxMap colorBoxMap; //QMultiMap< int, ColorBox >
  colorBoxMap.insert( firstBoxPixelSum, firstBox );
  QMap<int, QgsColorBox>::iterator colorBoxMapIt = colorBoxMap.end();

  //split boxes until number of boxes == nColors or all the boxes have color count 1
  bool allColorsMapped = false;
  while ( colorBoxMap.size() < nColors )
  {
    //start at the end of colorBoxMap and pick the first entry with number of colors < 1
    colorBoxMapIt = colorBoxMap.end();
    while ( true )
    {
      --colorBoxMapIt;
      if ( colorBoxMapIt.value().size() > 1 )
      {
        splitColorBox( colorBoxMapIt.value(), colorBoxMap, colorBoxMapIt );
        break;
      }
      if ( colorBoxMapIt == colorBoxMap.begin() )
      {
        allColorsMapped = true;
        break;
      }
    }

    if ( allColorsMapped )
    {
      break;
    }
    else
    {
      continue;
    }
  }

  //get representative colors for the boxes
  int index = 0;
  colorTable.resize( colorBoxMap.size() );
  QgsColorBoxMap::const_iterator colorBoxIt = colorBoxMap.constBegin();
  for ( ; colorBoxIt != colorBoxMap.constEnd(); ++colorBoxIt )
  {
    colorTable[index] = boxColor( colorBoxIt.value(), colorBoxIt.key() );
    ++index;
  }
}

void QgsHttpRequestHandler::imageColors( QHash<QRgb, int>& colors, const QImage& image )
{
  colors.clear();
  int width = image.width();
  int height = image.height();

  const QRgb* currentScanLine = nullptr;
  QHash<QRgb, int>::iterator colorIt;
  for ( int i = 0; i < height; ++i )
  {
    currentScanLine = ( const QRgb* )( image.scanLine( i ) );
    for ( int j = 0; j < width; ++j )
    {
      colorIt = colors.find( currentScanLine[j] );
      if ( colorIt == colors.end() )
      {
        colors.insert( currentScanLine[j], 1 );
      }
      else
      {
        colorIt.value()++;
      }
    }
  }
}

void QgsHttpRequestHandler::splitColorBox( QgsColorBox& colorBox, QgsColorBoxMap& colorBoxMap,
    QMap<int, QgsColorBox>::iterator colorBoxMapIt )
{

  if ( colorBox.size() < 2 )
  {
    return; //need at least two colors for a split
  }

  //a,r,g,b ranges
  int redRange = 0;
  int greenRange = 0;
  int blueRange = 0;
  int alphaRange = 0;

  if ( !minMaxRange( colorBox, redRange, greenRange, blueRange, alphaRange ) )
  {
    return;
  }

  //sort color box for a/r/g/b
  if ( redRange >= greenRange && redRange >= blueRange && redRange >= alphaRange )
  {
    qSort( colorBox.begin(), colorBox.end(), redCompare );
  }
  else if ( greenRange >= redRange && greenRange >= blueRange && greenRange >= alphaRange )
  {
    qSort( colorBox.begin(), colorBox.end(), greenCompare );
  }
  else if ( blueRange >= redRange && blueRange >= greenRange && blueRange >= alphaRange )
  {
    qSort( colorBox.begin(), colorBox.end(), blueCompare );
  }
  else
  {
    qSort( colorBox.begin(), colorBox.end(), alphaCompare );
  }

  //get median
  double halfSum = colorBoxMapIt.key() / 2.0;
  int currentSum = 0;
  int currentListIndex = 0;

  QgsColorBox::iterator colorBoxIt = colorBox.begin();
  for ( ; colorBoxIt != colorBox.end(); ++colorBoxIt )
  {
    currentSum += colorBoxIt->second;
    if ( currentSum >= halfSum )
    {
      break;
    }
    ++currentListIndex;
  }

  if ( currentListIndex > ( colorBox.size() - 2 ) ) //if the median is contained in the last color, split one item before that
  {
    --currentListIndex;
    currentSum -= colorBoxIt->second;
  }
  else
  {
    ++colorBoxIt; //the iterator needs to point behind the last item to remove
  }

  //do split: replace old color box, insert new one
  QgsColorBox newColorBox1 = colorBox.mid( 0, currentListIndex + 1 );
  colorBoxMap.insert( currentSum, newColorBox1 );

  colorBox.erase( colorBox.begin(), colorBoxIt );
  QgsColorBox newColorBox2 = colorBox;
  colorBoxMap.erase( colorBoxMapIt );
  colorBoxMap.insert( halfSum * 2.0 - currentSum, newColorBox2 );
}

bool QgsHttpRequestHandler::minMaxRange( const QgsColorBox& colorBox, int& redRange, int& greenRange, int& blueRange, int& alphaRange )
{
  if ( colorBox.size() < 1 )
  {
    return false;
  }

  int rMin = INT_MAX;
  int gMin = INT_MAX;
  int bMin = INT_MAX;
  int aMin = INT_MAX;
  int rMax = INT_MIN;
  int gMax = INT_MIN;
  int bMax = INT_MIN;
  int aMax = INT_MIN;

  int currentRed = 0;
  int currentGreen = 0;
  int currentBlue = 0;
  int currentAlpha = 0;

  QgsColorBox::const_iterator colorBoxIt = colorBox.constBegin();
  for ( ; colorBoxIt != colorBox.constEnd(); ++colorBoxIt )
  {
    currentRed = qRed( colorBoxIt->first );
    if ( currentRed > rMax )
    {
      rMax = currentRed;
    }
    if ( currentRed < rMin )
    {
      rMin = currentRed;
    }

    currentGreen = qGreen( colorBoxIt->first );
    if ( currentGreen > gMax )
    {
      gMax = currentGreen;
    }
    if ( currentGreen < gMin )
    {
      gMin = currentGreen;
    }

    currentBlue = qBlue( colorBoxIt->first );
    if ( currentBlue > bMax )
    {
      bMax = currentBlue;
    }
    if ( currentBlue < bMin )
    {
      bMin = currentBlue;
    }

    currentAlpha = qAlpha( colorBoxIt->first );
    if ( currentAlpha > aMax )
    {
      aMax = currentAlpha;
    }
    if ( currentAlpha < aMin )
    {
      aMin = currentAlpha;
    }
  }

  redRange = rMax - rMin;
  greenRange = gMax - gMin;
  blueRange = bMax - bMin;
  alphaRange = aMax - aMin;
  return true;
}

bool QgsHttpRequestHandler::redCompare( QPair<QRgb, int> c1, QPair<QRgb, int> c2 )
{
  return qRed( c1.first ) < qRed( c2.first );
}

bool QgsHttpRequestHandler::greenCompare( QPair<QRgb, int> c1, QPair<QRgb, int> c2 )
{
  return qGreen( c1.first ) < qGreen( c2.first );
}

bool QgsHttpRequestHandler::blueCompare( QPair<QRgb, int> c1, QPair<QRgb, int> c2 )
{
  return qBlue( c1.first ) < qBlue( c2.first );
}

bool QgsHttpRequestHandler::alphaCompare( QPair<QRgb, int> c1, QPair<QRgb, int> c2 )
{
  return qAlpha( c1.first ) < qAlpha( c2.first );
}

QRgb QgsHttpRequestHandler::boxColor( const QgsColorBox& box, int boxPixels )
{
  double avRed = 0;
  double avGreen = 0;
  double avBlue = 0;
  double avAlpha = 0;
  QRgb currentColor;
  int currentPixel;

  double weight;

  QgsColorBox::const_iterator colorBoxIt = box.constBegin();
  for ( ; colorBoxIt != box.constEnd(); ++colorBoxIt )
  {
    currentColor = colorBoxIt->first;
    currentPixel = colorBoxIt->second;
    weight = ( double )currentPixel / boxPixels;
    avRed += ( qRed( currentColor ) * weight );
    avGreen += ( qGreen( currentColor ) * weight );
    avBlue += ( qBlue( currentColor ) * weight );
    avAlpha += ( qAlpha( currentColor ) * weight );
  }

  return qRgba( avRed, avGreen, avBlue, avAlpha );
}


