// Copyright 2013 Velodyne Acoustics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkVelodyneHDLReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkVelodyneHDLReader.h"

#include "vtkNew.h"
#include "vtkSmartPointer.h"
#include "vtkCellData.h"
#include "vtkCellArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkPoints.h"
#include "vtkDoubleArray.h"
#include "vtkUnsignedShortArray.h"
#include "vtkUnsignedIntArray.h"
#include "vtkDataArray.h"
#include "vtkFloatArray.h"

#include "vtkPolyData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkMath.h"
#include "vtkStreamingDemandDrivenPipeline.h"

#include "vtkPacketFileReader.h"
#include "vtkPacketFileWriter.h"

#include <sstream>
#include <algorithm>
#include <cmath>


#include <boost/foreach.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>


#ifdef _MSC_VER
# include <boost/cstdint.hpp>
typedef boost::uint8_t uint8_t;
# ifndef M_PI
#   define M_PI 3.14159265358979323846
# endif
#else
# include <stdint.h>
#endif

namespace
{

#define HDL_Grabber_toRadians(x) ((x) * M_PI / 180.0)

const int HDL_NUM_ROT_ANGLES = 36001;
const int HDL_LASER_PER_FIRING = 32;
const int HDL_MAX_NUM_LASERS = 64;
const int HDL_FIRING_PER_PKT = 12;

enum HDLBlock
{
  BLOCK_0_TO_31 = 0xeeff,
  BLOCK_32_TO_63 = 0xddff
};

#pragma pack(push, 1)
typedef struct HDLLaserReturn
{
  unsigned short distance;
  unsigned char intensity;
} HDLLaserReturn;
#pragma pack(pop)

struct HDLFiringData
{
  unsigned short blockIdentifier;
  unsigned short rotationalPosition;
  HDLLaserReturn laserReturns[HDL_LASER_PER_FIRING];
};

struct HDLDataPacket
{
  HDLFiringData firingData[HDL_FIRING_PER_PKT];
  unsigned int gpsTimestamp;
  unsigned char blank1;
  unsigned char blank2;
};

struct HDLLaserCorrection
{
  double azimuthCorrection;
  double verticalCorrection;
  double distanceCorrection;
  double verticalOffsetCorrection;
  double horizontalOffsetCorrection;
  double sinVertCorrection;
  double cosVertCorrection;
  double sinVertOffsetCorrection;
  double cosVertOffsetCorrection;
};

struct HDLRGB
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

double *cos_lookup_table_;
double *sin_lookup_table_;
HDLLaserCorrection laser_corrections_[HDL_MAX_NUM_LASERS];
}

//-----------------------------------------------------------------------------
class vtkVelodyneHDLReader::vtkInternal
{
public:

  vtkInternal()
  {
    this->Skip = 0;
    this->LastAzimuth = 0;
    this->Reader = 0;
    this->Init();
  }

  std::vector<vtkSmartPointer<vtkPolyData> > Datasets;
  vtkSmartPointer<vtkPolyData> CurrentDataset;


  vtkPoints* Points;
  vtkUnsignedCharArray* Intensity;
  vtkUnsignedCharArray* LaserId;
  vtkUnsignedShortArray* Azimuth;
  vtkDoubleArray*        Distance;
  vtkUnsignedIntArray* Timestamp;


  unsigned int LastAzimuth;

  std::vector<fpos_t> FilePositions;
  std::vector<int> Skips;
  int Skip;
  vtkPacketFileReader* Reader;

  void SplitFrame();
  vtkSmartPointer<vtkPolyData> CreateData(vtkIdType numberOfPoints);
  vtkSmartPointer<vtkCellArray> NewVertexCells(vtkIdType numberOfVerts);

  void LoadHDL32Corrections();
  void LoadCorrectionsFile(const std::string& filename);
  void SetCorrectionsCommon();
  void Init();
  void InitTables();
  void ProcessHDLPacket(unsigned char *data, std::size_t bytesReceived);
};

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkVelodyneHDLReader);

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::vtkVelodyneHDLReader()
{
  this->Internal = new vtkInternal;
  this->UnloadData();
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);
}

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::~vtkVelodyneHDLReader()
{
  delete this->Internal;
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLReader::GetFileName()
{
  return this->FileName;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetFileName(const std::string& filename)
{
  if (filename == this->FileName)
    {
    return;
    }

  this->FileName = filename;
  this->Internal->FilePositions.clear();
  this->Internal->Skips.clear();
  this->UnloadData();
  this->Modified();
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLReader::GetCorrectionsFile()
{
  return this->CorrectionsFile;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetCorrectionsFile(const std::string& correctionsFile)
{
  if (correctionsFile == this->CorrectionsFile)
    {
    return;
    }

  if (correctionsFile.length())
    {
    this->Internal->LoadCorrectionsFile(correctionsFile);
    }
  else
    {
    this->Internal->LoadHDL32Corrections();
    }

  this->CorrectionsFile = correctionsFile;
  this->UnloadData();
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::UnloadData()
{
  this->Internal->LastAzimuth = 0;
  this->Internal->Datasets.clear();
  this->Internal->CurrentDataset = this->Internal->CreateData(0);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetTimestepInformation(vtkInformation *info)
{
  const size_t numberOfTimesteps = this->Internal->FilePositions.size();
  std::vector<double> timesteps;
  for (size_t i = 0; i < numberOfTimesteps; ++i)
    {
    timesteps.push_back(i);
    }

  if (numberOfTimesteps)
    {
    double timeRange[2] = {timesteps.front(), timesteps.back()};
    info->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), &timesteps.front(), timesteps.size());
    info->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);
    }
  else
    {
    info->Remove(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    info->Remove(vtkStreamingDemandDrivenPipeline::TIME_RANGE());
    }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::RequestData(vtkInformation *request,
                              vtkInformationVector **inputVector,
                              vtkInformationVector *outputVector)
{
  vtkPolyData *output = vtkPolyData::GetData(outputVector);
  vtkInformation *info = outputVector->GetInformationObject(0);

  if (!this->FileName.length())
    {
    vtkErrorMacro("FileName has not been set.");
    return 0;
    }


  int timestep = 0;
  if (info->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS()))
    {
    double timeRequest = info->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS())[0];
    timestep = static_cast<int>(floor(timeRequest+0.5));
    }

  if (timestep < 0 || timestep >= this->GetNumberOfFrames())
    {
    vtkErrorMacro("Cannot meet timestep request: " << timestep << ".  Have " << this->GetNumberOfFrames() << " datasets.");
    output->ShallowCopy(this->Internal->CreateData(0));
    return 0;
    }

  this->Open();
  output->ShallowCopy(this->GetFrame(timestep));
  this->Close();
  return 1;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::RequestInformation(vtkInformation *request,
                                     vtkInformationVector **inputVector,
                                     vtkInformationVector *outputVector)
{
  if (this->FileName.length() && !this->Internal->FilePositions.size())
    {
    this->ReadFrameInformation();
    }

  vtkInformation *info = outputVector->GetInformationObject(0);
  this->SetTimestepInformation(info);
  return 1;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "FileName: " << this->FileName << endl;
  os << indent << "CorrectionsFile: " << this->CorrectionsFile << endl;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::CanReadFile(const char *fname)
{
  return 1;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::ProcessHDLPacket(unsigned char *data, unsigned int bytesReceived)
{
  this->Internal->ProcessHDLPacket(data, bytesReceived);
}

//-----------------------------------------------------------------------------
std::vector<vtkSmartPointer<vtkPolyData> >& vtkVelodyneHDLReader::GetDatasets()
{
  return this->Internal->Datasets;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::GetNumberOfFrames()
{
  return this->Internal->FilePositions.size();;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::Open()
{
  this->Close();
  this->Internal->Reader = new vtkPacketFileReader;
  if (!this->Internal->Reader->Open(this->FileName))
    {
    vtkErrorMacro("Failed to open packet file: " << this->FileName << endl << this->Internal->Reader->GetLastError());
    this->Close();
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::Close()
{
  delete this->Internal->Reader;
  this->Internal->Reader = 0;
}


//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::DumpFrames(int startFrame, int endFrame, const std::string& filename)
{
  if (!this->Internal->Reader)
    {
    vtkErrorMacro("DumpFrames() called but packet file reader is not open.");
    return;
    }

  vtkPacketFileWriter writer;
  if (!writer.Open(filename))
    {
    vtkErrorMacro("Failed to open packet file for writing: " << filename);
    return;
    }

  pcap_pkthdr* header = 0;
  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  unsigned int lastAzimuth = 0;
  int currentFrame = startFrame;

  this->Internal->Reader->SetFilePosition(&this->Internal->FilePositions[startFrame]);
  int skip = this->Internal->Skips[startFrame];

  while (this->Internal->Reader->NextPacket(data, dataLength, timeSinceStart, &header) &&
         currentFrame <= endFrame)
    {
    if (dataLength == (1206 + 42))
      {
      writer.WritePacket(header, const_cast<unsigned char*>(data));
      }

    // Check if we cycled a frame and decrement
    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket *>(data + 42);

    for (int i = skip; i < HDL_FIRING_PER_PKT; ++i)
      {
      HDLFiringData firingData = dataPacket->firingData[i];

      if (firingData.rotationalPosition != 0 && firingData.rotationalPosition < lastAzimuth)
        {
        currentFrame++;
        if(currentFrame > endFrame)
          {
          break;
          }
        }
      lastAzimuth = firingData.rotationalPosition;
      }
    skip = 0;
    }

  writer.Close();
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::GetFrame(int frameNumber)
{
  this->UnloadData();
  if (!this->Internal->Reader)
    {
    vtkErrorMacro("GetFrame() called but packet file reader is not open.");
    return 0;
    }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;


  this->Internal->Reader->SetFilePosition(&this->Internal->FilePositions[frameNumber]);
  this->Internal->Skip = this->Internal->Skips[frameNumber];

  while (this->Internal->Reader->NextPacket(data, dataLength, timeSinceStart))
    {
    this->ProcessHDLPacket(const_cast<unsigned char*>(data), dataLength);

    if (this->Internal->Datasets.size())
      {
      return this->Internal->Datasets.back();
      }
    }

  this->Internal->SplitFrame();
  return this->Internal->Datasets.back();
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::vtkInternal::CreateData(vtkIdType numberOfPoints)
{
  vtkSmartPointer<vtkPolyData> polyData = vtkSmartPointer<vtkPolyData>::New();

  // points
  vtkNew<vtkPoints> points;
  points->SetDataTypeToFloat();
  points->SetNumberOfPoints(numberOfPoints);
  polyData->SetPoints(points.GetPointer());
  polyData->SetVerts(NewVertexCells(numberOfPoints));

  // intensity
  vtkNew<vtkUnsignedCharArray> intensity;
  intensity->SetName("intensity");
  intensity->SetNumberOfTuples(numberOfPoints);
  polyData->GetPointData()->AddArray(intensity.GetPointer());

  // laser number
  vtkNew<vtkUnsignedCharArray> laserId;
  laserId->SetName("laser_id");
  laserId->SetNumberOfTuples(numberOfPoints);
  polyData->GetPointData()->AddArray(laserId.GetPointer());

  // azimuth
  vtkNew<vtkUnsignedShortArray> azimuth;
  azimuth->SetName("azimuth");
  azimuth->SetNumberOfTuples(numberOfPoints);
  polyData->GetPointData()->AddArray(azimuth.GetPointer());

  // range
  vtkNew<vtkDoubleArray> distance;
  distance->SetName("distance_m");
  distance->SetNumberOfTuples(numberOfPoints);
  polyData->GetPointData()->AddArray(distance.GetPointer());

  // timestamp
  vtkNew<vtkUnsignedIntArray> timestamp;
  timestamp->SetName("timestamp");
  timestamp->SetNumberOfTuples(numberOfPoints);
  polyData->GetPointData()->AddArray(timestamp.GetPointer());

  this->Points = points.GetPointer();
  this->Intensity = intensity.GetPointer();
  this->LaserId = laserId.GetPointer();
  this->Azimuth = azimuth.GetPointer();
  this->Distance = distance.GetPointer();
  this->Timestamp = timestamp.GetPointer();

  return polyData;
}

//----------------------------------------------------------------------------
vtkSmartPointer<vtkCellArray> vtkVelodyneHDLReader::vtkInternal::NewVertexCells(vtkIdType numberOfVerts)
{
  vtkNew<vtkIdTypeArray> cells;
  cells->SetNumberOfValues(numberOfVerts*2);
  vtkIdType* ids = cells->GetPointer(0);
  for (vtkIdType i = 0; i < numberOfVerts; ++i)
    {
    ids[i*2] = 1;
    ids[i*2+1] = i;
    }

  vtkSmartPointer<vtkCellArray> cellArray = vtkSmartPointer<vtkCellArray>::New();
  cellArray->SetCells(numberOfVerts, cells.GetPointer());
  return cellArray;
}


namespace
{
void PushFiringData(vtkPolyData* polyData, unsigned char laserId, unsigned short azimuth, unsigned int timestamp, HDLLaserReturn laserReturn, HDLLaserCorrection correction, vtkVelodyneHDLReader::vtkInternal* internal)
{
  double cosAzimuth, sinAzimuth;
  if (correction.azimuthCorrection == 0)
  {
    cosAzimuth = cos_lookup_table_[azimuth];
    sinAzimuth = sin_lookup_table_[azimuth];
  }
  else
  {
    double azimuthInRadians = HDL_Grabber_toRadians((static_cast<double> (azimuth) / 100.0) - correction.azimuthCorrection);
    cosAzimuth = std::cos (azimuthInRadians);
    sinAzimuth = std::sin (azimuthInRadians);
  }

  double distanceM = laserReturn.distance * 0.002 + correction.distanceCorrection;
  double xyDistance = distanceM * correction.cosVertCorrection - correction.sinVertOffsetCorrection;

  double x = (xyDistance * sinAzimuth - correction.horizontalOffsetCorrection * cosAzimuth);
  double y = (xyDistance * cosAzimuth + correction.horizontalOffsetCorrection * sinAzimuth);
  double z = (distanceM * correction.sinVertCorrection + correction.cosVertOffsetCorrection);
  unsigned char intensity = laserReturn.intensity;

  internal->Points->InsertNextPoint(x,y,z);
  internal->Intensity->InsertNextValue(intensity);
  internal->LaserId->InsertNextValue(laserId);
  internal->Azimuth->InsertNextValue(azimuth);
  internal->Distance->InsertNextValue(distanceM);
  internal->Timestamp->InsertNextValue(timestamp);
}

}


//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::InitTables()
{
  if (cos_lookup_table_ == NULL && sin_lookup_table_ == NULL)
    {
    cos_lookup_table_ = static_cast<double *> (malloc (HDL_NUM_ROT_ANGLES * sizeof (*cos_lookup_table_)));
    sin_lookup_table_ = static_cast<double *> (malloc (HDL_NUM_ROT_ANGLES * sizeof (*sin_lookup_table_)));
    for (unsigned int i = 0; i < HDL_NUM_ROT_ANGLES; i++)
      {
      double rad = HDL_Grabber_toRadians(i / 100.0);
      cos_lookup_table_[i] = std::cos(rad);
      sin_lookup_table_[i] = std::sin(rad);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::LoadCorrectionsFile(const std::string& correctionsFile)
{

  boost::property_tree::ptree pt;
  try
    {
    read_xml(correctionsFile, pt, boost::property_tree::xml_parser::trim_whitespace);
    }
  catch (boost::exception const&)
    {
    vtkGenericWarningMacro("LoadCorrectionsFile: error reading calibration file: " << correctionsFile);
    return;
    }

  BOOST_FOREACH (boost::property_tree::ptree::value_type &v, pt.get_child("boost_serialization.DB.points_"))
    {
    if (v.first == "item")
      {
      boost::property_tree::ptree points = v.second;
      BOOST_FOREACH (boost::property_tree::ptree::value_type &px, points)
        {
        if (px.first == "px")
          {
          boost::property_tree::ptree calibrationData = px.second;
          int index = -1;
          double azimuth = 0;
          double vertCorrection = 0;
          double distCorrection = 0;
          double vertOffsetCorrection = 0;
          double horizOffsetCorrection = 0;

          BOOST_FOREACH (boost::property_tree::ptree::value_type &item, calibrationData)
            {
            if (item.first == "id_")
              index = atoi(item.second.data().c_str());
            if (item.first == "rotCorrection_")
              azimuth = atof(item.second.data().c_str());
            if (item.first == "vertCorrection_")
              vertCorrection = atof(item.second.data().c_str());
            if (item.first == "distCorrection_")
              distCorrection = atof(item.second.data().c_str());
            if (item.first == "vertOffsetCorrection_")
              vertOffsetCorrection = atof(item.second.data().c_str());
            if (item.first == "horizOffsetCorrection_")
              horizOffsetCorrection = atof(item.second.data().c_str());
            }
          if (index != -1)
            {
            laser_corrections_[index].azimuthCorrection = azimuth;
            laser_corrections_[index].verticalCorrection = vertCorrection;
            laser_corrections_[index].distanceCorrection = distCorrection / 100.0;
            laser_corrections_[index].verticalOffsetCorrection = vertOffsetCorrection / 100.0;
            laser_corrections_[index].horizontalOffsetCorrection = horizOffsetCorrection / 100.0;

            laser_corrections_[index].cosVertCorrection = std::cos (HDL_Grabber_toRadians(laser_corrections_[index].verticalCorrection));
            laser_corrections_[index].sinVertCorrection = std::sin (HDL_Grabber_toRadians(laser_corrections_[index].verticalCorrection));
            }
          }
        }
      }
    }

  this->SetCorrectionsCommon();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::LoadHDL32Corrections()
{
  double hdl32VerticalCorrections[] = {
    -30.67, -9.3299999, -29.33, -8, -28,
    -6.6700001, -26.67, -5.3299999, -25.33, -4, -24, -2.6700001, -22.67,
    -1.33, -21.33, 0, -20, 1.33, -18.67, 2.6700001, -17.33, 4, -16, 5.3299999,
    -14.67, 6.6700001, -13.33, 8, -12, 9.3299999, -10.67, 10.67 };

  for (int i = 0; i < HDL_LASER_PER_FIRING; i++)
    {
    laser_corrections_[i].azimuthCorrection = 0.0;
    laser_corrections_[i].distanceCorrection = 0.0;
    laser_corrections_[i].horizontalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalCorrection = hdl32VerticalCorrections[i];
    laser_corrections_[i].sinVertCorrection = std::sin (HDL_Grabber_toRadians(hdl32VerticalCorrections[i]));
    laser_corrections_[i].cosVertCorrection = std::cos (HDL_Grabber_toRadians(hdl32VerticalCorrections[i]));
    }

  for (int i = HDL_LASER_PER_FIRING; i < HDL_MAX_NUM_LASERS; i++)
    {
    laser_corrections_[i].azimuthCorrection = 0.0;
    laser_corrections_[i].distanceCorrection = 0.0;
    laser_corrections_[i].horizontalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalOffsetCorrection = 0.0;
    laser_corrections_[i].verticalCorrection = 0.0;
    laser_corrections_[i].sinVertCorrection = 0.0;
    laser_corrections_[i].cosVertCorrection = 1.0;
    }

  this->SetCorrectionsCommon();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::SetCorrectionsCommon()
{
  for (int i = 0; i < HDL_MAX_NUM_LASERS; i++)
    {
    HDLLaserCorrection correction = laser_corrections_[i];
    laser_corrections_[i].sinVertOffsetCorrection = correction.verticalOffsetCorrection
                                       * correction.sinVertCorrection;
    laser_corrections_[i].cosVertOffsetCorrection = correction.verticalOffsetCorrection
                                       * correction.cosVertCorrection;
    }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::Init()
{
  this->InitTables();
  this->LoadHDL32Corrections();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::SplitFrame()
{
  this->CurrentDataset->SetVerts(this->NewVertexCells(this->CurrentDataset->GetNumberOfPoints()));
  this->Datasets.push_back(this->CurrentDataset);
  this->CurrentDataset = this->CreateData(0);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ProcessHDLPacket(unsigned char *data, std::size_t bytesReceived)
{
  if (bytesReceived != 1206)
    {
    return;
    }

  HDLDataPacket* dataPacket = reinterpret_cast<HDLDataPacket *>(data);


  int i = this->Skip;
  this->Skip = 0;

  for ( ; i < HDL_FIRING_PER_PKT; ++i)
    {
    HDLFiringData firingData = dataPacket->firingData[i];
    int offset = (firingData.blockIdentifier == BLOCK_0_TO_31) ? 0 : 32;

    if (firingData.rotationalPosition < this->LastAzimuth
        )
        //&& this->CurrentDataset->GetNumberOfPoints())
      {
      this->SplitFrame();
      }

    this->LastAzimuth = firingData.rotationalPosition;

    for (int j = 0; j < HDL_LASER_PER_FIRING; j++)
      {
      unsigned char laserId = static_cast<unsigned char>(j + offset);
      if (firingData.laserReturns[j].distance != 0.0)
        {
        PushFiringData(this->CurrentDataset, laserId, firingData.rotationalPosition,
          dataPacket->gpsTimestamp, firingData.laserReturns[j], laser_corrections_[j + offset], this);
        }
      }
    }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::ReadFrameInformation()
{
  vtkPacketFileReader reader;
  if (!reader.Open(this->FileName))
    {
    vtkErrorMacro("Failed to open packet file: " << this->FileName << endl << reader.GetLastError());
    return 0;
    }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  unsigned int lastAzimuth = 0;
  unsigned int lastTimestamp = 0;

  std::vector<fpos_t> filePositions;
  std::vector<int> skips;

  fpos_t lastFilePosition;
  reader.GetFilePosition(&lastFilePosition);


  filePositions.push_back(lastFilePosition);
  skips.push_back(0);

  while (reader.NextPacket(data, dataLength, timeSinceStart))
    {

    if (dataLength != 1206)
      {
      continue;
      }

    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket *>(data);

    unsigned int timeDiff = dataPacket->gpsTimestamp - lastTimestamp;
    if (timeDiff > 600 && lastTimestamp != 0)
      {
      printf("missed %d packets\n",  static_cast<int>(floor((timeDiff/553.0) + 0.5)));
      }

    for (int i = 0; i < HDL_FIRING_PER_PKT; ++i)
      {
      HDLFiringData firingData = dataPacket->firingData[i];

      if (firingData.rotationalPosition < lastAzimuth)
        {
        filePositions.push_back(lastFilePosition);
        skips.push_back(i);
        this->UpdateProgress(0.0);
        }

      lastAzimuth = firingData.rotationalPosition;
      }

    lastTimestamp = dataPacket->gpsTimestamp;
    reader.GetFilePosition(&lastFilePosition);
    }

  this->Internal->FilePositions = filePositions;
  this->Internal->Skips = skips;
  return this->GetNumberOfFrames();
}
