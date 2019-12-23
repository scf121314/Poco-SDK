// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "PocoManager.h"
#include "PocoSDK.h"
#include "Misc/Converter.h"
#include "Misc/Parser.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UE4Node.h"
#include "UE4Dumper.h"

namespace Poco
{
	typedef TJsonWriterFactory< TCHAR, TCondensedJsonPrintPolicy<TCHAR> > FCondensedJsonStringWriterFactory;
	typedef TJsonWriter< TCHAR, TCondensedJsonPrintPolicy<TCHAR> > FCondensedJsonStringWriter;

	typedef TJsonWriterFactory< TCHAR, TPrettyJsonPrintPolicy<TCHAR> > FPrettyJsonStringWriterFactory;
	typedef TJsonWriter< TCHAR, TPrettyJsonPrintPolicy<TCHAR> > FPrettyJsonStringWriter;

	FPocoManager::FPocoManager()
	{
		TickableObject = new FTickableObject(this);
	}

	FPocoManager::~FPocoManager()
	{
	}

	void FPocoManager::Tick(float DeltaTime)
	{
		// Removes all finished worker threads.
		WorkerPool.RemoveAll([](TSharedPtr<FPocoWorker> Worker) {
			return Worker->SafeToDelete();
			});
	}

	void FPocoManager::HandleConnection(FSocket* InSocket)
	{		

		TSharedPtr<FPocoWorker> Worker = MakeShareable(new FPocoWorker(InSocket));

		WorkerPool.Add(Worker);

		Worker->Start();
	}

	FPocoWorker::FPocoWorker(FSocket* InSocket)
		: Started(false)
		, Ended(false)
		, Socket(InSocket)
	{
		Response = MakeShareable(new FJsonObject);
		
		Stopping = false;
	}

	uint32 FPocoWorker::Run()
	{
		FTimespan SleepTime = FTimespan::FromSeconds(1);

		while (!Stopping)
		{
			HandleRequest();

			SendResponse();

			FPlatformProcess::Sleep(SleepTime.GetSeconds());
		}

		return 0;
	}

	void FPocoWorker::Stop()
	{
		Stopping = true;
	}

	void FPocoWorker::Exit()
	{
		Ended = true;
	}

	void FPocoWorker::Start()
	{
		FRunnableThread::Create(this, TEXT("Poco worker thread"), 8 * 1024, TPri_Normal);
	}

	bool FPocoWorker::SafeToDelete()
	{
		return Started && Ended;
	}

	bool FPocoWorker::HandleRequest()
	{

		FString Request;

		if (!GetJsonString(Socket, Request))
		{
			UE_LOG(PocoLog, Error, TEXT("Failed to get request."));
			
			return false;
		}

		FString Method;

		if (!FJsonParser::GetMethod(Request, Method))
		{
			UE_LOG(PocoLog, Error, TEXT("Failed to read command with request %s."), *Request);

			return false;
		}
		else if (Method.Equals(TEXT("GetSDKVersion"), ESearchCase::IgnoreCase))
		{
			Response->SetStringField("result", "1.0");
		}
		else if (Method.Equals(TEXT("Dump"), ESearchCase::IgnoreCase))
		{
			UE4Dumper* Dumper = new UE4Dumper();
			TSharedPtr<FJsonObject> Result = Dumper->DumpHierarchy();

			Response->SetObjectField("result", Result);
		}

		if (!FJsonParser::GetId(Request, Id))
		{
			UE_LOG(PocoLog, Error, TEXT("Failed to read id with request %s."), *Request);
			return false;
		}

		return true;
	}

	bool FPocoWorker::SendResponse()
	{
		// Set jsonrpc
		Response->SetStringField("jsonrpc", "2.0");

		// Set id
		Response->SetStringField("id", Id);

		FString Output;

		TSharedRef<FCondensedJsonStringWriter> Writer = FCondensedJsonStringWriterFactory::Create(&Output);
		FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

		FTCHARToUTF8 ResponseBin(*Output);

		int32 SendSize = 0;
		int32 TotalSend = 0;
		int32 Length = ResponseBin.Length();

		bool result = Socket->Send(reinterpret_cast<const uint8*>(&Length), 4, SendSize);

		if (!result)
		{
			return false;
		}

		SendSize = 0;

		while (TotalSend < Length) {
			result = Socket->Send(reinterpret_cast<const uint8*>(ResponseBin.Get()) + TotalSend, Length, SendSize);
			if (!result)
			{
				return false;
			}
			TotalSend += SendSize;
		}
		return true;
	}
}